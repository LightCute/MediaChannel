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

    for (size_t i = 0; i < data.size() - 4; i++) {
        if ((ptr[i]==0 && ptr[i+1]==0 && ptr[i+2]==1) ||
            (ptr[i]==0 && ptr[i+1]==0 && ptr[i+2]==0 && ptr[i+3]==1)) {

            size_t offset = (ptr[i+2]==1) ? 3 : 4;
            uint8_t nal = ptr[i + offset] & 0x1F;

            Log::info("[NALU] type=[{}]", nal);
            switch(nal) {
                case 7: Log::info("(SPS)"); break;
                case 8: Log::info("(PPS)"); break;
                case 5: Log::info("(IDR slice)"); break;
                case 1: Log::info("(non-IDR slice)"); break;
                default: Log::info("(other)"); break;
            }
        }
    }
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