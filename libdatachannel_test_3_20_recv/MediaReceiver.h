#ifndef MEDIARECEIVER_H
#define MEDIARECEIVER_H

#include <memory>
#include <mutex>
#include "rtc/rtc.hpp"
#include "GstMediaPlayer.h"
#include "utilities/log.h"

class MediaReceiver {
public:
    MediaReceiver() = default;
    ~MediaReceiver();

    // 核心接口：添加音视频Track
    void addTrack(std::shared_ptr<rtc::Track> track, bool isVideo);
    // 绑定播放器
    void setPlayer(std::shared_ptr<GstMediaPlayer> player);
    // 停止释放资源
    void stop();

private:
    // 处理接收到的音视频帧
    void handleFrame(rtc::binary data, rtc::FrameInfo info, bool isVideo);
    // 初始化Track回调（打开/关闭/帧数据）
    void initTrackCallback(std::shared_ptr<rtc::Track> track, bool isVideo);

private:
    std::shared_ptr<rtc::Track> m_videoTrack;
    std::shared_ptr<rtc::Track> m_audioTrack;
    std::shared_ptr<GstMediaPlayer> m_player;

    std::mutex m_mutex;
    bool m_running = true;
};

#endif // MEDIARECEIVER_H