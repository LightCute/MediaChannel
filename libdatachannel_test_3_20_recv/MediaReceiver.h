#ifndef MEDIARECEIVER_H
#define MEDIARECEIVER_H

#include <memory>
#include <mutex>
#include <vector>
#include "rtc/rtc.hpp"
#include "GstMediaPlayer.h"
#include "utilities/log.h"

class MediaReceiver {
public:
    MediaReceiver();  
    ~MediaReceiver();

    void start() { 
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = true; // 启动！
        Log::info("[MediaReceiver] Started");
    }
    // 核心接口：添加音视频Track
    void addTrack(std::shared_ptr<rtc::Track> track, bool isVideo);
    // 绑定播放器
    void setPlayer(std::shared_ptr<GstMediaPlayer> player);
    // 停止释放资源
    void stop();

private:
    // 初始化Track回调（打开/关闭/帧数据）
    void initTrackCallback(std::shared_ptr<rtc::Track> track, bool isVideo);
    void debugH264(const rtc::binary& data);
private:
    std::shared_ptr<rtc::Track> m_videoTrack;
    std::shared_ptr<rtc::Track> m_audioTrack;
    std::shared_ptr<GstMediaPlayer> m_player;
    rtc::binary m_h264Buffer;
    std::mutex m_mutex;
    bool m_running = false;
};

#endif // MEDIARECEIVER_H