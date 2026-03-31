#pragma once

#include <gst/gst.h>
#include <thread>
#include <atomic>
#include <mutex>
#include "rtc/rtc.hpp"
#include "ThreadSafeQueue.h"

class GstMediaPlayer {
public:
    GstMediaPlayer();
    ~GstMediaPlayer();

    bool start();
    void stop();

    // 推入视频帧 (H.264)
    void pushVideoFrame(rtc::binary data, uint32_t timestamp);
    
    // 推入音频帧 (Opus 或 PCM，这里假设是 Opus)
    void pushAudioFrame(rtc::binary data, uint32_t timestamp);

private:
    // 静态探针回调
    static void onHandoff(GstElement* identity, GstBuffer* buffer, gpointer user_data);
    // 总线消息回调
    static gboolean onGstMessage(GstBus* bus, GstMessage* msg, gpointer user_data);
    // 内部推送函数
    void pushToAppSrc(GstElement* appsrc, const rtc::binary& data, uint32_t timestamp, uint32_t clockRate);
    GstElement* m_pipeline;
    GstElement* m_appsrcVideo;
    GstElement* m_appsrcAudio;
    
    std::atomic<bool> m_isRunning;
};