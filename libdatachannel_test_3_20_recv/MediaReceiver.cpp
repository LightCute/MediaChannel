#include "MediaReceiver.h"

MediaReceiver::~MediaReceiver() {
    stop();
}

void MediaReceiver::setPlayer(std::shared_ptr<GstMediaPlayer> player) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_player = player;
}

void MediaReceiver::addTrack(std::shared_ptr<rtc::Track> track, bool isVideo) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running || !track) return;

    // 1. 保存Track
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

    // 2. 初始化所有回调（解耦到独立函数）
    initTrackCallback(track, isVideo);
}

void MediaReceiver::initTrackCallback(std::shared_ptr<rtc::Track> track, bool isVideo) {
    // Track 打开
    track->onOpen([this, isVideo]() {
        Log::info("[MediaReceiver] {} Track Opened", isVideo ? "Video" : "Audio");
    });

    // Track 关闭
    track->onClosed([this, isVideo]() {
        Log::info("[MediaReceiver] {} Track Closed", isVideo ? "Video" : "Audio");
    });

    // 核心：帧数据接收（全部交给MediaReceiver处理）
    track->onFrame([this, isVideo](rtc::binary data, rtc::FrameInfo info) {
        handleFrame(std::move(data), info, isVideo);
    });
}

void MediaReceiver::handleFrame(rtc::binary data, rtc::FrameInfo info, bool isVideo) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running || !m_player || data.empty()) return;

    // 日志
    if (isVideo) {
        static int v_cnt = 0;
        if (++v_cnt % 30 == 0)
            Log::debug("[MediaReceiver] Received Video Frame, size={}", data.size());
    } else {
        Log::warn("[MediaReceiver] Received Audio Frame, size={}", data.size());
    }

    // 分发给独立播放器（零耦合调用）
    if (isVideo) {
        m_player->pushVideoFrame(std::move(data), info.timestamp);
    } else {
        m_player->pushAudioFrame(std::move(data), info.timestamp);
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