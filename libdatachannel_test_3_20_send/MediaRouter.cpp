#include "MediaRouter.h"
#include <algorithm>
#include "utilities/log.h"


MediaRouter::MediaRouter() {
    Log::info("[MediaRouter] Instance created");
}

MediaRouter::~MediaRouter() {
    stop();
    Log::info("[MediaRouter] Instance destroyed");
}

void MediaRouter::start() {
    if (m_running.exchange(true)) {
        Log::warn("[MediaRouter] Sender thread is already running");
        return;
    }

    m_thread = std::thread(&MediaRouter::senderLoop, this);
    Log::info("[MediaRouter] Sender thread started");
}

void MediaRouter::stop() {
    if (!m_running.exchange(false)) return;

    Log::info("[MediaRouter] Stopping sender thread...");
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    Log::info("[MediaRouter] Sender thread stopped");
}

void MediaRouter::pushFrame(const VideoFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        // 丢帧策略
        if (m_queue.size() > 10) {
            m_queue.pop();
            Log::warn("[MediaRouter] Queue overflow (max 10), dropped old frame");
        }

        m_queue.push(frame);
        //Log::debug("[MediaRouter] Pushed frame to queue, current queue size: {}", m_queue.size());
    }
    m_cv.notify_one();
}

void MediaRouter::registerClient(std::shared_ptr<ClientTrackData> client) {
    std::lock_guard<std::mutex> lock(m_clientMutex);
    m_clients.push_back(client);
    Log::info("[MediaRouter] Registered new client, total clients: {}", m_clients.size());

    // 新客户端补帧
    try {
        if (!m_cachedSpsPps.empty()) {
            client->track->sendFrame(m_cachedSpsPps, rtc::FrameInfo(0));
            Log::info("[MediaRouter] Sent cached SPS/PPS to new client");
        } else {
            Log::warn("[MediaRouter] No SPS/PPS cache available for new client");
        }

        if (!m_cachedIdr.empty()) {
            uint32_t ts = static_cast<uint32_t>(m_cachedIdrTs * 90 / 1000);
            client->track->sendFrame(m_cachedIdr, rtc::FrameInfo(ts));
            Log::info("[MediaRouter] Sent cached IDR to new client");
        } else {
            Log::warn("[MediaRouter] No IDR cache available for new client");
        }
    } catch (...) {
        Log::error("[MediaRouter] Failed to send cached data to new client");
    }
}

void MediaRouter::unregisterClient(std::shared_ptr<ClientTrackData> client) {
    std::lock_guard<std::mutex> lock(m_clientMutex);

    auto it = std::remove(m_clients.begin(), m_clients.end(), client);
    if (it != m_clients.end()) {
        m_clients.erase(it, m_clients.end());
        Log::info("[MediaRouter] Unregistered client, remaining clients: {}", m_clients.size());
    } else {
        Log::warn("[MediaRouter] Client not found for unregister");
    }
}

void MediaRouter::senderLoop() {
    Log::info("[MediaRouter] Sender loop running");
    while (m_running) {
        VideoFrame frame;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [&]() {
                return !m_running || !m_queue.empty();
            });

            if (!m_running) break;

            frame = std::move(m_queue.front());
            m_queue.pop();
            //Log::debug("[MediaRouter] Popped frame from queue, size: {} bytes", frame.data.size());
        }

        if (frame.data.empty()) {
            Log::warn("[MediaRouter] Skipped empty frame");
            continue;
        }

        // 解析并缓存关键帧
        parseAndCache(frame);

        // RTP 时间戳
        uint32_t rtp_ts = static_cast<uint32_t>(frame.timestamp_us * 90 / 1000);

        std::vector<std::shared_ptr<ClientTrackData>> clientsCopy;
        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            clientsCopy = m_clients;
        }

        if (clientsCopy.empty()) {
            Log::debug("[MediaRouter] No connected clients, skip sending frame");
            continue;
        }

        // 广播帧
        for (auto& client : clientsCopy) {
            try {
                rtc::FrameInfo info(rtp_ts);
                client->track->sendFrame(frame.data, info);
                client->sender->rtpConfig->timestamp = rtp_ts;
            } catch (const std::exception& e) {
                Log::error("[MediaRouter] Failed to send frame to client: {}", e.what());
            } catch (...) {
                Log::error("[MediaRouter] Unknown error when sending frame to client");
            }
        }
    }
    Log::info("[MediaRouter] Sender loop exited");
}

void MediaRouter::parseAndCache(const VideoFrame& frame) {
    const auto& data = frame.data;
    if (data.size() < 5) {
        Log::debug("[MediaRouter] Frame too small to parse NALU");
        return;
    }

    size_t offset = 0;
    while (offset + 4 <= data.size()) {
        uint32_t size =
            (static_cast<uint32_t>(data[offset])  << 24) |
            (static_cast<uint32_t>(data[offset+1])<< 16) |
            (static_cast<uint32_t>(data[offset+2])<< 8)  |
            (static_cast<uint32_t>(data[offset+3]));

        offset += 4;
        if (offset + size > data.size()) break;

        const uint8_t* nalu = reinterpret_cast<const uint8_t*>(data.data() + offset);
        uint8_t nalType = nalu[0] & 0x1F;

        rtc::binary singleNalu(4 + size);
        singleNalu[0] = std::byte( (size >> 24) & 0xFF );
        singleNalu[1] = std::byte( (size >> 16) & 0xFF );
        singleNalu[2] = std::byte( (size >> 8)  & 0xFF );
        singleNalu[3] = std::byte( size & 0xFF );
        std::copy(nalu, nalu + size, reinterpret_cast<uint8_t*>(singleNalu.data() + 4));

        // 缓存日志
        if (nalType == 7) {
            m_sps = singleNalu;
            Log::info("[MediaRouter] Parsed and cached SPS (NAL type=7)");
        }
        else if (nalType == 8) {
            m_pps = singleNalu;
            Log::info("[MediaRouter] Parsed and cached PPS (NAL type=8)");
        }
        else if (nalType == 5) {
            m_cachedIdr = singleNalu;
            m_cachedIdrTs = frame.timestamp_us;
            Log::info("[MediaRouter] Parsed and cached IDR (NAL type=5), timestamp={} us", m_cachedIdrTs);
        }

        offset += size;
    }

    // 拼接SPS+PPS
    if (!m_sps.empty() && !m_pps.empty() && m_cachedSpsPps.empty()) {
        m_cachedSpsPps.clear();
        m_cachedSpsPps.insert(m_cachedSpsPps.end(), m_sps.begin(), m_sps.end());
        m_cachedSpsPps.insert(m_cachedSpsPps.end(), m_pps.begin(), m_pps.end());
        Log::info("[MediaRouter] SPS + PPS combined and cached successfully");
    }
}