#include "GstMediaPlayer.h"
#include <iostream>
#include "utilities/log.h"
#include <gst/gst.h>

// 静态帧计数器（用于日志打印）
static int frame_cnt = 0;

GstMediaPlayer::GstMediaPlayer() 
    : m_pipeline(nullptr), m_appsrcVideo(nullptr), m_appsrcAudio(nullptr), m_isRunning(false) {
    gst_init(nullptr, nullptr);
    Log::info("[GstMediaPlayer] Constructor initialized, GStreamer inited");
}

GstMediaPlayer::~GstMediaPlayer() {
    stop();
}

// 静态探针回调函数（探测数据流）
void GstMediaPlayer::onHandoff(GstElement* identity, GstBuffer* buffer, gpointer user_data) {
    (void)identity;
    (void)user_data;
    Log::debug("[GstMediaPlayer] Data probe got buffer size={} bytes", gst_buffer_get_size(buffer));
}

bool GstMediaPlayer::start() {
    if (m_isRunning) {
        Log::warn("[GstMediaPlayer] Already running, skip start");
        return true;
    }

    Log::info("[GstMediaPlayer] Starting pipeline...");

    // ===================== 插入 identity 数据探针（解码链路探测器） =====================
    std::string pipelineDesc = 
        // 视频支路 + 探针
        "appsrc name=video_src emit-signals=false is-live=true ! "
        "queue max-size-buffers=0 max-size-time=0 max-size-bytes=0 ! "
        "identity silent=false name=probe1 ! "  // 探针1：appsrc输出
        "h264parse ! "
        "avdec_h264 ! "
        "identity silent=false name=probe2 ! "  // 探针2：解析后数据
        "videoconvert ! autovideosink sync=false "
        
        // 音频支路
        "appsrc name=audio_src emit-signals=false is-live=true ! "
        "queue max-size-buffers=0 max-size-time=0 max-size-bytes=0 ! "
        "opusparse ! opusdec ! audioconvert ! audioresample ! autoaudiosink sync=false ";

    Log::debug("[GstMediaPlayer] Pipeline desc: {}", pipelineDesc);

    GError* error = nullptr;
    m_pipeline = gst_parse_launch(pipelineDesc.c_str(), &error);
    if (error) {
        Log::error("[GstMediaPlayer] Pipeline parse error: {}", error->message);
        g_error_free(error);
        return false;
    }

    Log::info("[GstMediaPlayer] Pipeline created successfully");

    // 获取 AppSrc
    m_appsrcVideo = gst_bin_get_by_name(GST_BIN(m_pipeline), "video_src");
    m_appsrcAudio = gst_bin_get_by_name(GST_BIN(m_pipeline), "audio_src");

    if (!m_appsrcVideo || !m_appsrcAudio) {
        Log::error("[GstMediaPlayer] Failed to get appsrc (video={}, audio={})",
            m_appsrcVideo != nullptr, m_appsrcAudio != nullptr);
        return false;
    }

    Log::info("[GstMediaPlayer] AppSrc acquired successfully");

    // ===================== 设置视频 Caps =====================
    Log::info("[GstMediaPlayer] Setting video caps: H264 avc/au");
    GstCaps* videoCaps = gst_caps_from_string(
        "video/x-h264, stream-format=byte-stream, alignment=au"
    );
    g_object_set(m_appsrcVideo, "caps", videoCaps, nullptr);
    gst_caps_unref(videoCaps);

    // ===================== 设置音频 Caps =====================
    Log::info("[GstMediaPlayer] Setting audio caps: opus 48k stereo");
    GstCaps* audioCaps = gst_caps_new_simple(
        "audio/x-opus",
        "channels", G_TYPE_INT, 2,
        "rate", G_TYPE_INT, 48000,
        nullptr
    );
    g_object_set(m_appsrcAudio, "caps", audioCaps, nullptr);
    gst_caps_unref(audioCaps);

    // ===================== 连接数据探针信号 =====================
    GstElement* probe1 = gst_bin_get_by_name(GST_BIN(m_pipeline), "probe1");
    GstElement* probe2 = gst_bin_get_by_name(GST_BIN(m_pipeline), "probe2");
    if (probe1) {
        g_signal_connect(probe1, "handoff", G_CALLBACK(onHandoff), this);
        gst_object_unref(probe1);
    }
    if (probe2) {
        g_signal_connect(probe2, "handoff", G_CALLBACK(onHandoff), this);
        gst_object_unref(probe2);
    }
    Log::info("[GstMediaPlayer] Data probes connected successfully");

    // ===================== 启动管道 =====================
    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    Log::info("[GstMediaPlayer] Set pipeline PLAYING, ret={}", static_cast<int>(ret));

    // ===================== 总线监听 =====================
    GstBus* bus = gst_element_get_bus(m_pipeline);
    gst_bus_add_watch(bus, (GstBusFunc)GstMediaPlayer::onGstMessage, this);
    gst_object_unref(bus);

    m_isRunning = true;
    Log::info("[GstMediaPlayer] Started successfully (A/V Sync disabled for testing)");
    return true;
}

// 增强版 GStreamer 消息监听
gboolean GstMediaPlayer::onGstMessage(GstBus* bus, GstMessage* msg, gpointer user_data) {
    (void)bus;
    GstMediaPlayer* self = (GstMediaPlayer*)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        // 错误信息
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug;
            gst_message_parse_error(msg, &err, &debug);
            Log::error("[GstMediaPlayer] ERROR: {} | Debug: {}", err->message, debug);
            g_error_free(err);
            g_free(debug);
            break;
        }
        // 警告信息
        case GST_MESSAGE_WARNING: {
            GError* err;
            gchar* debug;
            gst_message_parse_warning(msg, &err, &debug);
            Log::warn("[GstMediaPlayer] WARNING: {} | Debug: {}", err->message, debug);
            g_error_free(err);
            g_free(debug);
            break;
        }
        // 普通信息
        case GST_MESSAGE_INFO: {
            GError* err;
            gchar* debug;
            gst_message_parse_info(msg, &err, &debug);
            Log::info("[GstMediaPlayer] INFO: {}", err->message);
            g_error_free(err);
            g_free(debug);
            break;
        }
        // 管道状态变化
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->m_pipeline)) {
                GstState old_state, new_state, pending;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
                Log::info("[GstMediaPlayer] Pipeline state changed: {} -> {}",
                    gst_element_state_get_name(old_state),
                    gst_element_state_get_name(new_state));
            }
            break;
        }
        // 流结束
        case GST_MESSAGE_EOS:
            Log::warn("[GstMediaPlayer] Received EOS (stream ended)");
            break;

        default:
            break;
    }
    return TRUE;
}

void GstMediaPlayer::stop() {
    if (!m_isRunning) {
        Log::warn("[GstMediaPlayer] Already stopped, skip stop");
        return;
    }

    Log::info("[GstMediaPlayer] Stopping pipeline...");
    m_isRunning = false;

    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        m_appsrcVideo = nullptr;
        m_appsrcAudio = nullptr;
    }

    Log::info("[GstMediaPlayer] Stopped successfully");
}

void GstMediaPlayer::pushVideoFrame(rtc::binary data, uint32_t timestamp) {
    if (!m_isRunning || !m_appsrcVideo) return;
    pushToAppSrc(m_appsrcVideo, data, timestamp, 90000);
}

void GstMediaPlayer::pushAudioFrame(rtc::binary data, uint32_t timestamp) {
    if (!m_isRunning || !m_appsrcAudio) return;
    pushToAppSrc(m_appsrcAudio, data, timestamp, 48000);
}

// 增强版 数据推送函数（带完整日志）
void GstMediaPlayer::pushToAppSrc(GstElement* appsrc, const rtc::binary& data, uint32_t ts, uint32_t clock) {
    if (!appsrc || data.empty()) {
        Log::warn("[GstMediaPlayer] pushToAppSrc: invalid data");
        return;
    }

    GstBuffer* buf = gst_buffer_new_memdup(data.data(), data.size());
    GST_BUFFER_PTS(buf) = (uint64_t)ts * GST_SECOND / clock;
    GST_BUFFER_DTS(buf) = GST_BUFFER_PTS(buf);

    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
    
    // ✅ 添加日志，验证推送结果
    if (ret == GST_FLOW_OK) {
        Log::trace("[GstMediaPlayer] Pushed buffer size={}, ts={}", data.size(), ts);
    } else {
        Log::error("[GstMediaPlayer] Push buffer failed, ret={}", (int)ret);
    }

    gst_buffer_unref(buf);
}