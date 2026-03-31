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
        }

        if (frame.data.empty()) continue;

        // 1. 解析并缓存NALU（原有逻辑）
        parseAndCache(frame);

        // 2. ✅ 核心修复：从整帧中提取【所有单个NALU】
        std::vector<rtc::binary> nalus = splitAnnexBFrame(frame.data);
        if (nalus.empty()) continue;

        uint32_t rtp_ts = static_cast<uint32_t>(frame.timestamp_us * 90 / 1000);

        // 3. 复制客户端列表
        std::vector<std::shared_ptr<ClientTrackData>> clientsCopy;
        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            clientsCopy = m_clients;
        }
        if (clientsCopy.empty()) continue;

        // 4. ✅ 逐个发送【单个NALU】（符合RTP打包器要求）
        for (const auto& nalu : nalus) {
            for (auto& client : clientsCopy) {
                try {
                    rtc::FrameInfo info(rtp_ts);
                    // 发送单个NALU ✅ 正确用法
                    client->track->sendFrame(nalu, info);
                    client->sender->rtpConfig->timestamp = rtp_ts;
                } catch (const std::exception& e) {
                    Log::error("[MediaRouter] Send NALU failed: {}", e.what());
                }
            }
        }
    }
    Log::info("[MediaRouter] Sender loop exited");
}

// 新增：将Annex-B整帧拆分为单个NALU列表
std::vector<rtc::binary> MediaRouter::splitAnnexBFrame(const rtc::binary& data) {
    std::vector<rtc::binary> nalus;
    if (data.size() < 4) return nalus;

    std::vector<size_t> startCodes;
    for (size_t i = 0; i < data.size() - 3; i++) {
        if (data[i] == std::byte(0) && data[i+1] == std::byte(0) && 
            data[i+2] == std::byte(0) && data[i+3] == std::byte(1)) {
            startCodes.push_back(i);
            i += 3;
        } else if (data[i] == std::byte(0) && data[i+1] == std::byte(0) && 
                   data[i+2] == std::byte(1)) {
            startCodes.push_back(i);
            i += 2;
        }
    }

    for (size_t i = 0; i < startCodes.size(); i++) {
        size_t start = startCodes[i];
        size_t end = (i == startCodes.size()-1) ? data.size() : startCodes[i+1];
        if (end - start > 0) {
            nalus.emplace_back(data.begin() + start, data.begin() + end);
        }
    }
    return nalus;
}

void MediaRouter::parseAndCache(const VideoFrame& frame) {
    const auto& data = frame.data;
    if (data.size() < 4) {
        Log::debug("[MediaRouter] Frame too small to parse Annex-B NALU");
        return;
    }

    // Annex-B 起始码：0x000001 (3字节) 或 0x00000001 (4字节)
    std::vector<size_t> startCodes;
    for (size_t i = 0; i < data.size() - 3; i++) {
        // 匹配 0x000001
        if (data[i]   == std::byte(0x00) &&
            data[i+1] == std::byte(0x00) &&
            data[i+2] == std::byte(0x00) &&
            data[i+3] == std::byte(0x01)) {
            startCodes.push_back(i);
            i += 3; // 跳过4字节起始码
        }
        // 匹配 0x000001
        else if (data[i]   == std::byte(0x00) &&
                 data[i+1] == std::byte(0x00) &&
                 data[i+2] == std::byte(0x01)) {
            startCodes.push_back(i);
            i += 2; // 跳过3字节起始码
        }
    }

    if (startCodes.empty()) {
        Log::debug("[MediaRouter] No Annex-B start code found");
        return;
    }

    // 遍历所有 NALU（Annex-B 格式，直接保留原始数据，无需Length前缀）
    for (size_t i = 0; i < startCodes.size(); i++) {
        size_t start = startCodes[i];
        size_t end = (i == startCodes.size() - 1) ? data.size() : startCodes[i+1];
        size_t naluSize = end - start;

        if (naluSize < 1) continue;

        // 提取 NALU 原始数据（包含起始码，符合Annex-B）
        const uint8_t* nalu = reinterpret_cast<const uint8_t*>(data.data() + start);
        uint8_t nalType = nalu[0] & 0x1F;

        // 直接缓存原始 Annex-B 数据（打包器会自动处理）
        rtc::binary singleNalu(data.begin() + start, data.begin() + end);

        // 缓存 SPS/PPS/IDR
        if (nalType == 7) {
            m_sps = singleNalu;
            Log::info("[MediaRouter] Parsed and cached SPS (Annex-B, NAL type=7)");
        }
        else if (nalType == 8) {
            m_pps = singleNalu;
            Log::info("[MediaRouter] Parsed and cached PPS (Annex-B, NAL type=8)");
        }
        else if (nalType == 5) {
            m_cachedIdr = singleNalu;
            m_cachedIdrTs = frame.timestamp_us;
            Log::info("[MediaRouter] Parsed and cached IDR (Annex-B, NAL type=5)");
        }
    }

    // 拼接 SPS+PPS 缓存
    if (!m_sps.empty() && !m_pps.empty() && m_cachedSpsPps.empty()) {
        m_cachedSpsPps = m_sps;
        m_cachedSpsPps.insert(m_cachedSpsPps.end(), m_pps.begin(), m_pps.end());
        Log::info("[MediaRouter] SPS + PPS combined (Annex-B)");
    }
}