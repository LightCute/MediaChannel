#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include "MediaFrame.h"
#include <rtc/rtc.hpp>
#include "helpers.hpp"

class MediaRouter {
public:
    MediaRouter();
    ~MediaRouter();

    void start();
    void stop();

    void pushFrame(const VideoFrame& frame);

    void registerClient(std::shared_ptr<ClientTrackData> client);
    void unregisterClient(std::shared_ptr<ClientTrackData> client);

private:
    void senderLoop();

    void parseAndCache(const VideoFrame& frame);

private:
    std::vector<std::shared_ptr<ClientTrackData>> m_clients;
    std::mutex m_clientMutex;

    std::queue<VideoFrame> m_queue;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;

    std::thread m_thread;
    std::atomic<bool> m_running{false};



    rtc::binary m_cachedSpsPps;  // 拼接后的SPS+PPS
    rtc::binary m_cachedIdr;     // 缓存的IDR帧
    uint64_t m_cachedIdrTs{0};   // IDR时间戳
    rtc::binary m_sps;           // 单独SPS  【新增】
    rtc::binary m_pps;           // 单独PPS  【新增】

};