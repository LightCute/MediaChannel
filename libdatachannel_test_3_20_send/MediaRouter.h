#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <thread>
#include <optional>
#include "MediaFrame.h"
#include "helpers.hpp"
#include "utilities/blocking_queue.h"
#include "utilities/log.h"

class MediaRouter {
public:
    MediaRouter() = default;
    ~MediaRouter();

    // 启动/停止发送线程
    void start();
    void stop();

    // 核心接口：接收帧（FrameSource 回调调用）
    void onFrameReceived(const VideoFrame& frame);

    // 注册/注销客户端（新Peer连接时调用）
    void registerClient(std::weak_ptr<ClientTrackData> clientTrack);
    void unregisterClient(std::weak_ptr<ClientTrackData> clientTrack);

    // 设置SPS/PPS缓存
    void setSpsPps(const rtc::binary& spsPps);

private:
    // 发送线程主循环
    void senderLoop();
    // 广播帧到所有客户端
    void broadcastFrame(const VideoFrame& frame);
    // 新客户端补帧（SPS/PPS/IDR）
    void sendCachedDataToClient(std::shared_ptr<ClientTrackData> track);
    // 更新IDR缓存
    void updateIdrCache(const VideoFrame& frame);

private:
    std::atomic<bool> m_running{false};
    std::thread m_senderThread;
    BlockingQueue<VideoFrame> m_frameQueue;

    // 客户端管理（弱指针，无循环引用）
    std::mutex m_clientMutex;
    std::vector<std::weak_ptr<ClientTrackData>> m_clientList;

    // 缓存：补帧用
    std::mutex m_cacheMutex;
    rtc::binary m_cachedSpsPps;
    VideoFrame m_cachedIdrFrame;

    // 队列保护：最大缓存帧数
    static constexpr size_t MAX_QUEUE_SIZE = 10;
};