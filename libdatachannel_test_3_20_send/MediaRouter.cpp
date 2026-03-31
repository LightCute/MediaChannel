#include "MediaRouter.h"
#include <algorithm>

MediaRouter::MediaRouter() {}

MediaRouter::~MediaRouter() {
    stop();
}

void MediaRouter::start() {
    if (m_running.exchange(true)) return;

    m_thread = std::thread(&MediaRouter::senderLoop, this);
}

void MediaRouter::stop() {
    if (!m_running.exchange(false)) return;

    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void MediaRouter::pushFrame(const VideoFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        // 🔥 丢帧策略（关键）
        if (m_queue.size() > 10) {
            // 丢掉旧帧（保持实时）
            m_queue.pop();
        }

        m_queue.push(frame);
    }
    m_cv.notify_one();
}

void MediaRouter::registerClient(std::shared_ptr<ClientTrackData> client) {
    std::lock_guard<std::mutex> lock(m_clientMutex);
    m_clients.push_back(client);

    // 🔥 新客户端立即补 SPS/PPS + IDR
    try {
        if (!m_cachedSpsPps.empty()) {
            client->track->sendFrame(m_cachedSpsPps, rtc::FrameInfo(0));
        }

        if (!m_cachedIdr.empty()) {
            uint32_t ts = static_cast<uint32_t>(m_cachedIdrTs * 90 / 1000);
            client->track->sendFrame(m_cachedIdr, rtc::FrameInfo(ts));
        }
    } catch (...) {}
}

void MediaRouter::unregisterClient(std::shared_ptr<ClientTrackData> client) {
    std::lock_guard<std::mutex> lock(m_clientMutex);

    m_clients.erase(
        std::remove(m_clients.begin(), m_clients.end(), client),
        m_clients.end()
    );
}

void MediaRouter::senderLoop() {
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
        }

        if (frame.data.empty()) continue;

        // 🔥 缓存 SPS / PPS / IDR
        parseAndCache(frame);

        // RTP 时间戳
        uint32_t rtp_ts = static_cast<uint32_t>(frame.timestamp_us * 90 / 1000);

        std::vector<std::shared_ptr<ClientTrackData>> clientsCopy;
        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            clientsCopy = m_clients;
        }

        for (auto& client : clientsCopy) {
            try {
                rtc::FrameInfo info(rtp_ts);

                client->track->sendFrame(frame.data, info);

                // 更新 SR
                client->sender->rtpConfig->timestamp = rtp_ts;

            } catch (...) {}
        }
    }
}

void MediaRouter::parseAndCache(const VideoFrame& frame) {
    const auto& data = frame.data;
    if (data.size() < 5) return;

    size_t offset = 0;
    while (offset + 4 <= data.size()) {
        // 读取AVCC格式的NAL长度(大端)
        uint32_t size =
            (static_cast<uint32_t>(data[offset])  << 24) |
            (static_cast<uint32_t>(data[offset+1])<< 16) |
            (static_cast<uint32_t>(data[offset+2])<< 8)  |
            (static_cast<uint32_t>(data[offset+3]));

        offset += 4;
        if (offset + size > data.size()) break;

        const uint8_t* nalu = reinterpret_cast<const uint8_t*>(data.data() + offset);
        uint8_t nalType = nalu[0] & 0x1F;

        // 构造单NALU包（带4字节长度头）
        rtc::binary singleNalu(4 + size);

        // ===================== 核心修复：std::byte 必须显式强转 =====================
        singleNalu[0] = std::byte( (size >> 24) & 0xFF );
        singleNalu[1] = std::byte( (size >> 16) & 0xFF );
        singleNalu[2] = std::byte( (size >> 8)  & 0xFF );
        singleNalu[3] = std::byte( size & 0xFF );

        // 拷贝NALU数据
        std::copy(nalu, nalu + size, reinterpret_cast<uint8_t*>(singleNalu.data() + 4));

        // 缓存SPS/PPS/IDR
        if (nalType == 7) {
            m_sps = singleNalu;
        }
        else if (nalType == 8) {
            m_pps = singleNalu;
        }
        else if (nalType == 5) {
            m_cachedIdr = singleNalu;
            m_cachedIdrTs = frame.timestamp_us;
        }

        offset += size;
    }

    // 拼接SPS+PPS，用于新客户端快速出图
    if (!m_sps.empty() && !m_pps.empty()) {
        m_cachedSpsPps.clear();
        m_cachedSpsPps.insert(m_cachedSpsPps.end(), m_sps.begin(), m_sps.end());
        m_cachedSpsPps.insert(m_cachedSpsPps.end(), m_pps.begin(), m_pps.end());
    }
}