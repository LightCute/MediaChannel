// FrameSource.cpp
#include "FrameSource.h"
#include "utilities/log.h"
#include <stdexcept>

// 辅助宏：检查 GStreamer 元素是否创建成功
#define GST_CHECK_ELEMENT(elem, name) \
    if (!elem) { throw std::runtime_error("Could not create GStreamer element: " std::string(name)); }

// ==========================================
// 🎯 新增：通用 Probe 调试回调函数
// ==========================================
static GstPadProbeReturn probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        GstMapInfo map;

        // 获取探针的名字，方便区分是哪个位置的日志
        const char* probe_name = (const char*)user_data;

        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            Log::info("----------------------- [PROBE: {}] -----------------------", probe_name);
            Log::info("[PROBE: {}] Buffer size = {}", probe_name, map.size);

            // 简单 NALU 分析
            const uint8_t* ptr = map.data;
            int nal_count = 0;
            
            // 防止越界的安全检查
            if (map.size > 4) {
                for (size_t i = 0; i < map.size - 4; ) {
                    bool is_start_code = false;
                    size_t start_code_len = 0;

                    // 检查 4字节起始码 (00 00 00 01)
                    if (i + 4 <= map.size && 
                        ptr[i] == 0 && ptr[i+1] == 0 && ptr[i+2] == 0 && ptr[i+3] == 1) {
                        is_start_code = true;
                        start_code_len = 4;
                    }
                    // 检查 3字节起始码 (00 00 01)
                    else if (i + 3 <= map.size && 
                             ptr[i] == 0 && ptr[i+1] == 0 && ptr[i+2] == 1) {
                        is_start_code = true;
                        start_code_len = 3;
                    }

                    if (is_start_code) {
                        size_t nal_header_idx = i + start_code_len;
                        if (nal_header_idx < map.size) {
                            uint8_t nal_type = ptr[nal_header_idx] & 0x1F;
                            
                            std::string type_str;
                            switch(nal_type) {
                                case 9: type_str = "AUD"; break;
                                case 7: type_str = "SPS"; break;
                                case 8: type_str = "PPS"; break;
                                case 5: type_str = "IDR"; break;
                                case 1: type_str = "Non-IDR"; break;
                                case 6: type_str = "SEI"; break;
                                default: type_str = "Other(" + std::to_string(nal_type) + ")"; break;
                            }
                            
                            Log::info("[PROBE: {}]  -> NALU [{}] Type: {} ({})", probe_name, nal_count, nal_type, type_str);
                            nal_count++;
                        }
                        // 跳过起始码，继续找下一个
                        i += start_code_len + 1; 
                    } else {
                        i++;
                    }
                }
            }
            Log::info("[PROBE: {}] Total NALUs: {}", probe_name, nal_count);
            Log::info("-----------------------------------------------------------", probe_name);

            gst_buffer_unmap(buffer, &map);
        }
    }
    return GST_PAD_PROBE_OK;
}

// ==========================================
// 类成员函数实现
// ==========================================

FrameSource::FrameSource() 
    : m_running(false), m_pipeline(nullptr), m_appSink(nullptr), m_firstPts(0) {
    gst_init(nullptr, nullptr);
}

FrameSource::~FrameSource() {
    stop();
}

void FrameSource::setCallback(FrameCallback cb) {
    m_callback = std::move(cb);
}

rtc::binary FrameSource::getCachedSpsPps() const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_cachedSpsPps;
}

bool FrameSource::start() {
    if (m_running.exchange(true)) {
        Log::warn("[FrameSource] Already running");
        return true;
    }

    m_firstPts = 0;
    Log::info("[FrameSource] Starting GStreamer pipeline...");

    // --- 优化 1: Pipeline 描述字符串 ---
    // 给 x264enc 和 h264parse 加上了名字，方便后续获取指针
    const char* pipelineDesc = R"(
        v4l2src device=/dev/video0 ! 
        image/jpeg,width=640,height=480,framerate=30/1 ! 
        jpegdec ! 
        videoconvert ! 
        tee name=t

        t. ! queue leaky=downstream max-size-buffers=2 ! 
            x264enc name=my_encoder tune=zerolatency speed-preset=ultrafast bitrate=1000 key-int-max=30 bframes=0 aud=false ! 
            h264parse name=my_parser config-interval=-1 ! 
            video/x-h264,stream-format=byte-stream ! 
            appsink name=video_sink emit-signals=true sync=false

        t. ! queue ! 
            videoconvert ! 
            autovideosink sync=false
    )";

    GError* error = nullptr;
    m_pipeline = gst_parse_launch(pipelineDesc, &error);
    if (error) {
        Log::error("[FrameSource] Failed to parse pipeline: {}", error->message);
        g_error_free(error);
        m_running = false;
        return false;
    }

    // 获取 appsink
    m_appSink = gst_bin_get_by_name(GST_BIN(m_pipeline), "video_sink");
    if (!m_appSink) {
        Log::error("[FrameSource] Failed to get appsink");
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        m_running = false;
        return false;
    }

    // --- 优化 2: 挂载 Probe (关键步骤) ---
    // 1. 获取 Encoder 的 src pad (输出)
    GstElement* encoder = gst_bin_get_by_name(GST_BIN(m_pipeline), "my_encoder");
    if (encoder) {
        GstPad* pad = gst_element_get_static_pad(encoder, "src");
        if (pad) {
            // user_data 传入 "EncOut" 用于日志区分
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, probe_cb, (gpointer)"EncOut", NULL);
            gst_object_unref(pad);
        }
        gst_object_unref(encoder);
    }

    // 2. 获取 Parser 的 sink pad (输入)
    GstElement* parser = gst_bin_get_by_name(GST_BIN(m_pipeline), "my_parser");
    if (parser) {
        GstPad* pad_sink = gst_element_get_static_pad(parser, "sink");
        if (pad_sink) {
            gst_pad_add_probe(pad_sink, GST_PAD_PROBE_TYPE_BUFFER, probe_cb, (gpointer)"ParseIn", NULL);
            gst_object_unref(pad_sink);
        }
        
        // 3. 获取 Parser 的 src pad (输出)
        GstPad* pad_src = gst_element_get_static_pad(parser, "src");
        if (pad_src) {
            gst_pad_add_probe(pad_src, GST_PAD_PROBE_TYPE_BUFFER, probe_cb, (gpointer)"ParseOut", NULL);
            gst_object_unref(pad_src);
        }
        gst_object_unref(parser);
    }

    // 配置 Bus 消息监听
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    gst_bus_add_watch(bus, onBusMessageStatic, this);
    gst_object_unref(bus);

    // 配置 appsink 回调
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = onNewSampleStatic;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_appSink), &callbacks, this, nullptr);

    // 启动
    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        Log::error("[FrameSource] Failed to set pipeline to PLAYING");
        gst_object_unref(m_appSink);
        gst_object_unref(m_pipeline);
        m_pipeline = m_appSink = nullptr;
        m_running = false;
        return false;
    }

    Log::info("[FrameSource] Pipeline started successfully");
    return true;
}

void FrameSource::stop() {
    if (!m_running.exchange(false)) return;

    Log::info("[FrameSource] Stopping...");

    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        m_appSink = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cachedSpsPps.clear();
    }
    
    m_firstPts = 0;
    Log::info("[FrameSource] Stopped");
}

GstFlowReturn FrameSource::onNewSampleStatic(GstAppSink* sink, gpointer user_data) {
    return static_cast<FrameSource*>(user_data)->onNewSample(sink);
}

gboolean FrameSource::onBusMessageStatic(GstBus* bus, GstMessage* msg, gpointer user_data) {
    static_cast<FrameSource*>(user_data)->onBusMessage(msg);
    return TRUE;
}

void FrameSource::onBusMessage(GstMessage* msg) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug;
            gst_message_parse_error(msg, &err, &debug);
            Log::error("[FrameSource] GST Error: {} (Debug: {})", err->message, debug ? debug : "none");
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* err;
            gchar* debug;
            gst_message_parse_warning(msg, &err, &debug);
            Log::warn("[FrameSource] GST Warning: {}", err->message);
            g_error_free(err);
            g_free(debug);
            break;
        }
        default:
            break;
    }
}

GstFlowReturn FrameSource::onNewSample(GstAppSink* sink) {
    if (!m_running || !m_callback) return GST_FLOW_OK;

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // 1. 计算时间戳
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    uint64_t timestamp_us = 0;
    
    if (pts == GST_CLOCK_TIME_NONE) {
        pts = g_get_monotonic_time() * 1000; 
    }
    if (m_firstPts == 0) {
        m_firstPts = pts;
        timestamp_us = 0;
    } else {
        timestamp_us = (pts - m_firstPts) / 1000;
    }

    // 2. 封装数据
    VideoFrame frame;
    frame.data = rtc::binary(reinterpret_cast<std::byte*>(map.data), 
                              reinterpret_cast<std::byte*>(map.data + map.size));
    frame.timestamp_us = timestamp_us;
    frame.isIdr = false;
    if (map.size >= 5) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(map.data);
        for (size_t i = 0; i < map.size - 4; i++) {
            if ((ptr[i] == 0 && ptr[i+1] == 0 && ptr[i+2] == 0 && ptr[i+3] == 1) || 
                (ptr[i] == 0 && ptr[i+1] == 0 && ptr[i+2] == 1)) {
                uint8_t nal_type = ptr[i + (ptr[i+2]==1 ? 3 : 4)] & 0x1F;
                if (nal_type == 5) {
                    frame.isIdr = true;
                    break;
                }
            }
        }
    }
    Log::debug("[FrameSource] Captured frame: size={}, isIdr={}", map.size, frame.isIdr);
    
    // 注意：为了不刷屏，你可以选择性注释掉 debugH264，只看 Probe 日志
    debugH264(frame.data); 
    
    m_callback(frame);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    return GST_FLOW_OK;
}

void FrameSource::debugH264(const rtc::binary& data) {
    if (data.size() < 5) return;

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
    size_t size = data.size();

    int nalCount = 0;
    int audCount = 0;
    int sliceCount = 0;

    auto find_start_code = [&](size_t offset) -> size_t {
        for (size_t i = offset; i + 3 < size; ++i) {
            // 00 00 01
            if (ptr[i] == 0 && ptr[i+1] == 0 && ptr[i+2] == 1)
                return i;
            // 00 00 00 01
            if (i + 4 < size &&
                ptr[i] == 0 && ptr[i+1] == 0 &&
                ptr[i+2] == 0 && ptr[i+3] == 1)
                return i;
        }
        return size; // not found
    };

    size_t pos = find_start_code(0);

    while (pos < size) {
        // 判断 start code 长度
        size_t start_code_len = (ptr[pos+2] == 1) ? 3 : 4;
        size_t nal_start = pos + start_code_len;

        if (nal_start >= size) break;

        // 找下一个 start code
        size_t next = find_start_code(nal_start);

        size_t nal_end = (next < size) ? next : size;

        uint8_t nal_type = ptr[nal_start] & 0x1F;

        Log::info("[FrameSource][NALU] type={}", nal_type);

        switch(nal_type) {
            case 9:
                Log::info("  -> (AUD)");
                audCount++;
                break;
            case 7:
                Log::info("  -> (SPS)");
                break;
            case 8:
                Log::info("  -> (PPS)");
                break;
            case 6:
                Log::info("  -> (SEI)");
                break;
            case 5:
                Log::info("  -> (IDR slice)");
                sliceCount++;
                break;
            case 1:
                Log::info("  -> (non-IDR slice)");
                sliceCount++;
                break;
            default:
                Log::info("  -> (other)");
                break;
        }

        nalCount++;

        // 🚨 关键：跳到下一个NALU（不是 i++）
        pos = next;
    }

    Log::info("[FrameSource][SUMMARY] size={}, nalCount={}, audCount={}, sliceCount={}",
              size, nalCount, audCount, sliceCount);
}