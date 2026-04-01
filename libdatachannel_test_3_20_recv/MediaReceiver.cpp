#include "MediaReceiver.h"

MediaReceiver::MediaReceiver() = default;
MediaReceiver::~MediaReceiver() {
    stop();
}

void MediaReceiver::setPlayer(std::shared_ptr<GstMediaPlayer> player) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_player = player;
}

void MediaReceiver::addTrack(std::shared_ptr<rtc::Track> track, bool isVideo) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if(!track) return;
    initTrackCallback(track, isVideo);
    if (isVideo) {
        m_videoTrack = track;
        Log::info("[MediaReceiver] Added Video Track");
        // 创建H264解包器
        auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
        track->setMediaHandler(depacketizer);
    } else {
        m_audioTrack = track;
        Log::info("[MediaReceiver] Added Audio Track");
        // 创建Opus解包器
        auto depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
        track->setMediaHandler(depacketizer);
    }

   
}
void MediaReceiver::initTrackCallback(std::shared_ptr<rtc::Track> track, bool isVideo) {
    track->onOpen([this, isVideo]() {
        Log::info("[MediaReceiver] {} Track Opened", isVideo ? "Video" : "Audio");
    });

    track->onClosed([this, isVideo]() {
        Log::info("[MediaReceiver] {} Track Closed", isVideo ? "Video" : "Audio");
    });

    // 🔥 核心：data = 解包器自动重组的 完整H264/Opus帧
    track->onFrame([this, isVideo](rtc::binary data, rtc::FrameInfo info) {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 校验：运行状态+播放器+数据有效
        if (!m_running || !m_player || data.empty()) {
            return;
        }

        // 直接推送完整帧，无任何中间处理
        if (isVideo) {
            debugH264(data);
            Log::info("[MediaReceiver] receive H264 Frame | size: {} bytes", data.size());
            // 修复API错误：timestamp() 是函数调用！
            m_player->pushVideoFrame(data, info.timestamp);
        } else {
            Log::info("[MediaReceiver] receive Opus Frame | size: {} bytes", data.size());
            m_player->pushAudioFrame(data, info.timestamp);
        }
    });
}

void MediaReceiver::debugH264(const rtc::binary& data) {
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
void MediaReceiver::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running) return;

    m_running = false;

    // 释放Track
    if (m_videoTrack) m_videoTrack->close();
    if (m_audioTrack) m_audioTrack->close();
    m_videoTrack.reset();
    m_audioTrack.reset();

    // 释放播放器
    if (m_player) {
        m_player->stop();
        m_player.reset();
    }

    Log::info("[MediaReceiver] Stopped");
}