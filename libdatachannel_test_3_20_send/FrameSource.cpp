// FrameSource.cpp
#include "FrameSource.h"
#include "utilities/log.h"
#include <stdexcept>

// 辅助宏：检查 GStreamer 元素是否创建成功
#define GST_CHECK_ELEMENT(elem, name) \
    if (!elem) { throw std::runtime_error("Could not create GStreamer element: " std::string(name)); }

FrameSource::FrameSource() 
    : m_running(false), m_pipeline(nullptr), m_appSink(nullptr), m_firstPts(0) {
    // 注意：建议在 main() 函数里调用一次 gst_init，而不是在这里
    // 如果确定外部没初始化，可以在这里打开：
    // gst_init(nullptr, nullptr);
}

FrameSource::~FrameSource() {
    stop();
}

void FrameSource::setCallback(FrameCallback cb) {
    m_callback = std::move(cb);
}

rtc::binary FrameSource::getCachedSpsPps() const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_cachedSpsPps; // 返回拷贝
}

bool FrameSource::start() {
    if (m_running.exchange(true)) {
        Log::warn("[FrameSource] Already running");
        return true;
    }

    m_firstPts = 0;
    Log::info("[FrameSource] Starting GStreamer pipeline...");

    // --- Pipeline 描述字符串 ---
    // 注意：使用 avc 格式 (长度前缀)，这是 libdatachannel 通常需要的
    const char* pipelineDesc = R"(
        v4l2src device=/dev/video0 ! 
        image/jpeg,width=640,height=480,framerate=30/1 ! 
        jpegdec ! 
        videoconvert ! 
        queue leaky=downstream max-size-buffers=2 ! 
        x264enc tune=zerolatency speed-preset=ultrafast bitrate=1000 key-int-max=30 bframes=0 ! 
        h264parse config-interval=-1 ! 
        video/x-h264,stream-format=avc,alignment=au ! 
        appsink name=video_sink emit-signals=true sync=false
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

    // 清空缓存
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cachedSpsPps.clear();
    }
    
    m_firstPts = 0;
    Log::info("[FrameSource] Stopped");
}

// --- 静态回调转发 ---

GstFlowReturn FrameSource::onNewSampleStatic(GstAppSink* sink, gpointer user_data) {
    return static_cast<FrameSource*>(user_data)->onNewSample(sink);
}

gboolean FrameSource::onBusMessageStatic(GstBus* bus, GstMessage* msg, gpointer user_data) {
    static_cast<FrameSource*>(user_data)->onBusMessage(msg);
    return TRUE;
}

// --- 实际逻辑处理 ---

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

    // 3. 解析 NALU 并缓存 SPS/PPS
    parseAndCacheNalus(frame.data, frame);

    // 4. 推送回调 (注意：这里是在 GST 线程中调用的)
    m_callback(frame);

    // 5. 清理
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    return GST_FLOW_OK;
}

void FrameSource::parseAndCacheNalus(const rtc::binary& data, VideoFrame& frame) {
    if (data.size() < 5) return;

    size_t offset = 0;
    // 这里的逻辑假设是 AVCC (长度前缀) 格式
    // 如果你需要 Annex-B (00 00 00 01)，解析逻辑需要微调
    while (offset + 4 <= data.size()) {
        // 读取长度 (Big Endian)
        uint32_t nal_size =
            (uint32_t(data[offset]) << 24) |
            (uint32_t(data[offset + 1]) << 16) |
            (uint32_t(data[offset + 2]) << 8) |
            (uint32_t(data[offset + 3]));

        offset += 4;
        if (offset + nal_size > data.size()) break;

        uint8_t nal_header = uint8_t(data[offset]);
        uint8_t nal_type = nal_header & 0x1F;

        // 缓存 SPS (7) / PPS (8)
        if (nal_type == 7 || nal_type == 8) {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            // 简单的全帧缓存策略：如果这一帧包含 SPS/PPS，缓存整帧
            // 更精细的做法是只缓存 specific NALUs
            m_cachedSpsPps = data; 
        }

        // 标记 IDR (5)
        if (nal_type == 5) {
            frame.isIdr = true;
        }

        offset += nal_size;
    }
}