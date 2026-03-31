// FrameSource.h
#pragma once

#include "MediaFrame.h"
#include <functional>
#include <atomic>
#include <mutex>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

class FrameSource {
public:
    using FrameCallback = std::function<void(const VideoFrame&)>;

    FrameSource();
    ~FrameSource();

    // 禁止拷贝
    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;

    // 设置回调 (在 start() 之前调用)
    void setCallback(FrameCallback cb);

    // 启动采集 pipeline
    bool start();

    // 停止采集
    void stop();

    // 获取最近缓存的 SPS/PPS (供新连接快速初始化)
    rtc::binary getCachedSpsPps() const;

private:
    // GStreamer 内部回调
    static GstFlowReturn onNewSampleStatic(GstAppSink* sink, gpointer user_data);
    GstFlowReturn onNewSample(GstAppSink* sink);
    static gboolean onBusMessageStatic(GstBus* bus, GstMessage* msg, gpointer user_data);
    void onBusMessage(GstMessage* msg);

    // 解析 H.264 并更新缓存
    void parseAndCacheNalus(const rtc::binary& data, VideoFrame& frame);
    void debugH264(const rtc::binary& data);
    // 成员变量
    FrameCallback m_callback;
    std::atomic<bool> m_running;

    // GStreamer 资源
    GstElement* m_pipeline;
    GstElement* m_appSink;
    uint64_t m_firstPts;

    // 缓存保护
    mutable std::mutex m_cacheMutex;
    rtc::binary m_cachedSpsPps;
};