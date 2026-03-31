#include "MediaRouter.h"

MediaRouter::~MediaRouter() {
    stop();
}

void MediaRouter::start() {
    if (m_running.exchange(true)) return;
    m_senderThread = std::thread(&MediaRouter::senderLoop, this);
    Log::info("[MediaRouter] 发送线程启动");
}

void MediaRouter::stop() {
    if (!m_running.exchange(false)) return;
    m_frameQueue.stop();
    if (m_senderThread.joinable()) m_senderThread.join();
    Log::info("[MediaRouter] 发送线程停止");
}

void MediaRouter::setSpsPps(const rtc::binary& spsPps) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cachedSpsPps = spsPps;
}

void MediaRouter::updateIdrCache(const VideoFrame& frame) {
    if (frame.isIdr) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cachedIdrFrame = frame;
    }
}

void MediaRouter::onFrameReceived(const VideoFrame& frame) {
    if (!m_running) return;

    // 更新IDR缓存
    updateIdrCache(frame);

    // 队列保护，丢帧防内存溢出
    if (m_frameQueue.size() > MAX_QUEUE_SIZE) {
        Log::warn("[MediaRouter] 队列溢出，丢弃帧");
        return;
    }

    // 入队
    m_frameQueue.push(frame);
}

void MediaRouter::registerClient(std::weak_ptr<ClientTrackData> clientTrack) {
    std::lock_guard<std::mutex> lock(m_clientMutex);
    m_clientList.emplace_back(clientTrack);

    // 新客户端立即补帧
    if (auto track = clientTrack.lock()) {
        sendCachedDataToClient(track);
    }
}

void MediaRouter::unregisterClient(std::weak_ptr<ClientTrackData> clientTrack) {
    std::lock_guard<std::mutex> lock(m_clientMutex);
    for (auto it = m_clientList.begin(); it != m_clientList.end();) {
        if (it->lock() == clientTrack.lock()) {
            it = m_clientList.erase(it);
        } else {
            ++it;
        }
    }
}

void MediaRouter::sendCachedDataToClient(std::shared_ptr<ClientTrackData> track) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    try {
        // 发送SPS/PPS
        if (!m_cachedSpsPps.empty()) {
            rtc::FrameInfo info(0);
            track->track->sendFrame(m_cachedSpsPps, info);
            Log::info("[MediaRouter] 发送缓存SPS/PPS给新客户端");
        }
        // 发送IDR帧
        if (!m_cachedIdrFrame.data.empty()) {
            uint32_t rtpTs = static_cast<uint32_t>(m_cachedIdrFrame.timestamp_us * 90 / 1000);
            rtc::FrameInfo info(rtpTs);
            track->track->sendFrame(m_cachedIdrFrame.data, info);
            Log::info("[MediaRouter] 发送缓存IDR给新客户端");
        }
    } catch (const std::exception& e) {
        Log::error("[MediaRouter] 补帧失败: {}", e.what());
    }
}

void MediaRouter::broadcastFrame(const VideoFrame& frame) {
    std::lock_guard<std::mutex> lock(m_clientMutex);
    uint32_t rtpTs = static_cast<uint32_t>(frame.timestamp_us * 90 / 1000);

    // 遍历并清理失效客户端
    for (auto it = m_clientList.begin(); it != m_clientList.end();) {
        if (auto track = it->lock()) {
            try {
                rtc::FrameInfo info(rtpTs);
                track->track->sendFrame(frame.data, info);
                track->sender->rtpConfig->timestamp = rtpTs;
                ++it;
            } catch (...) {
                it = m_clientList.erase(it);
            }
        } else {
            it = m_clientList.erase(it);
        }
    }
}

void MediaRouter::senderLoop() {
    while (m_running) {
        auto frame = m_frameQueue.waitAndPop();
        if (!m_running || frame.data.empty()) continue;

        broadcastFrame(frame);
    }
}