#include "DataChannelClient.h"
#include <algorithm>
#include <chrono>
#include <random>
#include <stdexcept>
#include <thread>
#include "opusfileparser.hpp"


using namespace std::chrono_literals;
using nlohmann::json;

DataChannelClient::DataChannelClient() 
    : m_MainThread("MainThread")
{
    Log::init("app_send.log", Log::Mode::Async, spdlog::level::trace);
    Log::info("[DataChannelClient] Starting, thread id: [{}]", Log::threadIdToString(std::this_thread::get_id()));
    rtc::InitLogger(rtc::LogLevel::Debug, myCppLogCallback);
    m_localId = randomId(4);
    Log::info("[DataChannelClient] Local ID generated: {}", m_localId);
    m_config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    rtc::IceServer turnServer(
        "120.79.210.6",          // 纯IP，不带任何协议
        3478,                     // 端口
        "test",                   // 用户名（和网页一致）
        "123",                    // 密码（和网页一致）
        rtc::IceServer::RelayType::TurnUdp  // 强制UDP，去掉 ?transport=udp
    );
    //m_config.iceServers.push_back(turnServer);
    m_frameSource = std::make_unique<FrameSource>();
    m_mediaRouter = std::make_unique<MediaRouter>();
    m_frameSource->setCallback([this](const VideoFrame& frame) {
        // 这里的代码运行在 GStreamer 线程上
        // 你需要把 frame 放入队列，或者分发给所有 client
        m_mediaRouter->pushFrame(frame);
    });
}

// 启动流的地方
void DataChannelClient::startGStreamerPipeline() { // 这个函数名现在可以改了
    if (!m_frameSource->start()) {
        Log::error("Failed to start camera");
    }
}
void DataChannelClient::onFrameReceived(const VideoFrame& frame) {
    m_mediaRouter->pushFrame(frame);
}


DataChannelClient::~DataChannelClient() {
    close();
}

void DataChannelClient::connectToServer(const std::string& serverUrl) {
    if (m_ws && m_ws->isOpen()) {
        Log::warn("[WebSocket] WebSocket is already connected");
        return;
    }

    m_ws = std::make_shared<rtc::WebSocket>();
    m_wsPromise = std::promise<void>();
    auto wsFuture = m_wsPromise.get_future();

    m_ws->onOpen([this]() {
        Log::info("[WebSocket] WebSocket connected, signaling ready");
        m_wsPromise.set_value();
    });

    m_ws->onError([this](std::string s) {
        Log::error("[WebSocket] WebSocket error: {}", s);
        m_wsPromise.set_exception(std::make_exception_ptr(std::runtime_error(s)));
    });

    m_ws->onClosed([this]() {
        Log::info("[WebSocket] WebSocket closed");
    });

    m_ws->onMessage([this, wws = std::weak_ptr<rtc::WebSocket>(m_ws)](auto data) {
        if (!std::holds_alternative<std::string>(data)) return;

        json message = json::parse(std::get<std::string>(data));
        auto it = message.find("id");
        if (it == message.end()) return;
        auto id = it->get<std::string>();

        it = message.find("type");
        if (it == message.end()) return;
        auto type = it->get<std::string>();

        std::shared_ptr<rtc::PeerConnection> pc;
        if (auto jt = m_clients.find(id); jt != m_clients.end()) {
            pc = jt->second->peerConnection;
        } else if (type == "offer") {
            Log::info("[WebSocket] Answering to {}", id);
            pc = createPeerConnection(wws, id);
        } else {
            return;
        }
        if (type == "offer" || type == "answer") {
            auto sdp = message["description"].get<std::string>();
            pc->setRemoteDescription(rtc::Description(sdp, type));
        } else if (type == "candidate") {
            auto sdp = message["candidate"].get<std::string>();
            auto mid = message["mid"].get<std::string>();
            pc->addRemoteCandidate(rtc::Candidate(sdp, mid));
        }
    });

    std::string fullUrl = serverUrl + "/" + m_localId;
    m_ws->open(fullUrl);

    Log::info("[WebSocket] Waiting for signaling connection...");
    wsFuture.get();
    Log::info("[WebSocket] Waiting for signaling connected...");
}

void DataChannelClient::callPeer(const std::string& peerId) {
    if (!m_ws || !m_ws->isOpen()) {
        Log::error("[DataChannelClient] WebSocket is not connected");
        return;
    }

    if (peerId.empty() || peerId == m_localId) {
        Log::warn("[DataChannelClient] Invalid peer ID");
        return;
    }

    m_connectionPromisePtr = std::make_shared<std::promise<void>>();
    m_connectedFlag = false;
    Log::info("[DataChannelClient] Offering to {}", peerId);
    auto pc = createPeerConnection(m_ws, peerId);

    auto it_client = m_clients.find(peerId);
    if (it_client == m_clients.end()) {
        Log::error("[DataChannelClient] Failed to find or create Client for peer {}", peerId);
        return;
    }
    auto client = it_client->second;
    // 生成唯一 SSRC（可以用你的 generateUniqueSSRC 函数）
    uint32_t videoSsrc = generateUniqueSSRC(peerId + "_video");
    uint32_t audioSsrc = generateUniqueSSRC(peerId + "_audio");

    auto videoTrackData = addVideo(pc, 102, videoSsrc, "video-stream", "stream-" + peerId, nullptr);
    
    client->video = videoTrackData;

    videoTrackData->track->onOpen([this, peerId, client, videoTrackData]() {
        Log::info("[DataChannelClient] Video track open for {}, starting GStreamer", peerId);
        m_mediaRouter->registerClient(videoTrackData);
        m_mediaRouter->start();
        startGStreamerPipeline(); 
    });
    // client->audio = addAudio(pc, 111, audioSsrc, "audio-stream", "stream-" + peerId, [this, peerId, wc = make_weak_ptr(client)]() {
    //     m_MainThread.dispatch([this, wc, peerId]() {
    //         if (auto c = wc.lock()) {
    //             addToStream(c, false);
    //         }
    //     });
    //     Log::info("[DataChannelClient] Audio track for {} is open", peerId);
    // });

    const std::string label = "test";
    Log::info("[DataChannelClient] Creating DataChannel with label \"{}\"", label);
    auto dc = pc->createDataChannel(label);



    dc->onOpen([this, peerId, wdc = std::weak_ptr<rtc::DataChannel>(dc)]() {
        Log::info("[DataChannelClient] DataChannel from {} open", peerId);
        m_connectedFlag = true;
        if (m_connectionPromisePtr) {
            try {
                m_connectionPromisePtr->set_value();
            } catch (const std::future_error& e) {
                Log::warn("[DataChannelClient] Promise already set: {}", e.what());
            }
        }
        
        if (auto dc = wdc.lock()) dc->send("Hello from " + m_localId);
    });

    dc->onClosed([this, peerId]() {
        Log::info("[DataChannelClient] DataChannel from {} closed", peerId);
    });

    dc->onMessage([this, peerId](auto data) {
        if (std::holds_alternative<std::string>(data)) {
            Log::info("[DataChannelClient] Message from {}: {}", peerId, std::get<std::string>(data));
        } else {
            Log::info("[DataChannelClient] Binary message from {}, size={}", peerId, std::get<rtc::binary>(data).size());
        }
    });

    {
        client->dataChannel = dc;
    }
}

void DataChannelClient::sendMessage(const std::string& peerId, const std::string& message) {
    auto it = m_clients.find(peerId);
    if (it == m_clients.end()) {
        Log::warn("[DataChannelClient] No DataChannel for peer {}", peerId);
        return;
    }

    auto& client = it->second;
    if (client->dataChannel.has_value() && client->dataChannel.value()->isOpen()) {
        auto dc = client->dataChannel.value();
        if (dc->isOpen()) {
            dc->send(message);
            Log::info("[DataChannelClient] Sent to {}: {}", peerId, message);
        } else {
            Log::warn("[DataChannelClient] DataChannel for {} is not open", peerId);
        }
    } else {
        Log::warn("[DataChannelClient] DataChannel for {} is not open", peerId);
    }
}

void DataChannelClient::close() {
    Log::info("[DataChannelClient] Cleaning up...");
    

    m_frameSource->stop();
    m_mediaRouter->stop();
    for (auto& [id, client] : m_clients) {
        if (client->dataChannel.has_value() && client->dataChannel.value()) {
            client->dataChannel.value()->close();
        }
        if (client->peerConnection) {
            client->peerConnection->close();
        }
    }
    m_clients.clear();
    m_connectedFlag = false;
    if (m_ws) { m_ws->close(); m_ws.reset(); }
}

std::string DataChannelClient::getLocalId() const {
    return m_localId;
}



bool DataChannelClient::waitForConnection(std::chrono::milliseconds timeout) {
    if (m_connectedFlag) {
        Log::info("[DataChannelClient] Already connected!");
        return true;
    }
    
    if (!m_connectionPromisePtr) {
        Log::error("[DataChannelClient] No promise set!");
        return false;
    }
    
    auto future = m_connectionPromisePtr->get_future();
    return future.wait_for(timeout) == std::future_status::ready;
}

std::shared_ptr<rtc::PeerConnection> DataChannelClient::createPeerConnection(
    std::weak_ptr<rtc::WebSocket> wws, 
    const std::string& id
) {
    auto pc = std::make_shared<rtc::PeerConnection>(m_config);
    Log::info("[PeerConnection] Created PC for peer: {}", id);


    pc->onStateChange([](rtc::PeerConnection::State state) {
        Log::info("[PeerConnection] State: {}", peerConnectionStateToString(state));
    });

    pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
        Log::info("[PeerConnection] Gathering State: {}", gatheringStateToString(state));
    });

    pc->onLocalDescription([wws, id](rtc::Description description) {
        Log::info("[PeerConnection] [{}] Generated local {}: {} bytes", 
            id, description.typeString(), std::string(description).size());
        json message = {
            {"id", id},
            {"type", description.typeString()},
            {"description", std::string(description)}
        };
        if (auto ws = wws.lock()) {
            ws->send(message.dump());
            Log::info("[PeerConnection] [{}] Sent local description to signaling server", message.dump()); // 只打印前100字符
        }
    });

    pc->onLocalCandidate([wws, id](rtc::Candidate candidate) {
        Log::debug("[PeerConnection] [{}] ICE Candidate: {} (mid={})", 
                   id, std::string(candidate), candidate.mid());
        json message = {
            {"id", id},
            {"type", "candidate"},
            {"candidate", std::string(candidate)},
            {"mid", candidate.mid()}
        };
        if (auto ws = wws.lock()) ws->send(message.dump());
    });

    pc->onTrack([this, id](std::shared_ptr<rtc::Track> track) {
        Log::info("[PeerConnection] [{}] Received track: {}", 
            id, track->mid());        
                  
        // track->receive([this, id](rtc::binary data, uint32_t timestamp) {
        //     Log::info("[DataChannelClient] Received video frame from {}, size: {}, timestamp: {}", id, data.size(), timestamp);
        //     // 在此添加解码和显示逻辑（如FFmpeg/Qt渲染）
        // });
    });

    pc->onDataChannel([this, id](std::shared_ptr<rtc::DataChannel> dc) {
        Log::info("[PeerConnection] DataChannel from {} received: {}", id, dc->label());


        dc->onOpen([this, id, wdc = std::weak_ptr<rtc::DataChannel>(dc)]() {
            Log::info("[PeerConnection] DataChannel from {} open", id);
            m_connectedFlag = true;
            if (auto dc = wdc.lock()) dc->send("Hello from " + m_localId);
        });

        dc->onClosed([this, id]() {
            Log::info("[PeerConnection] DataChannel from {} closed", id);
        });

        dc->onMessage([this, id](auto data) {
            if (std::holds_alternative<std::string>(data)) {
                Log::info("[PeerConnection] Message from {}: {}", id, std::get<std::string>(data));
            } else {
                Log::info("[PeerConnection] Binary message from {}, size={}", id, std::get<rtc::binary>(data).size());
            }
        });

        auto it = m_clients.find(id);
        if (it != m_clients.end()) {
            it->second->dataChannel = dc;
        }
    });

    auto it = m_clients.find(id);
    if (it == m_clients.end()) {
        auto client = std::make_shared<Client>(pc);
        m_clients.emplace(id, client);
    }
    return pc;
}

std::string DataChannelClient::randomId(size_t length) {
    using std::chrono::high_resolution_clock;
    static thread_local std::mt19937 rng(
        static_cast<unsigned int>(high_resolution_clock::now().time_since_epoch().count())
    );
    static const std::string chars("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    std::string id(length, '0');
    std::uniform_int_distribution<int> dist(0, int(chars.size() - 1));
    std::generate(id.begin(), id.end(), [&]() { return chars.at(dist(rng)); });
    return id;
}

std::string DataChannelClient::peerConnectionStateToString(rtc::PeerConnection::State state) {
    switch (state) {
        case rtc::PeerConnection::State::New: return "New";
        case rtc::PeerConnection::State::Connecting: return "Connecting";
        case rtc::PeerConnection::State::Connected: return "Connected";
        case rtc::PeerConnection::State::Disconnected: return "Disconnected";
        case rtc::PeerConnection::State::Failed: return "Failed";
        case rtc::PeerConnection::State::Closed: return "Closed";
        default: return "Unknown";
    }
}

std::string DataChannelClient::gatheringStateToString(rtc::PeerConnection::GatheringState state) {
    switch (state) {
        case rtc::PeerConnection::GatheringState::New: return "New";
        case rtc::PeerConnection::GatheringState::InProgress: return "InProgress";
        case rtc::PeerConnection::GatheringState::Complete: return "Complete";
        default: return "Unknown";
    }
}

void DataChannelClient::myCppLogCallback(rtc::LogLevel level, std::string message) {
    switch (level) {
        case rtc::LogLevel::Fatal:
        case rtc::LogLevel::Error: Log::error("[RTC] {}", message); break;
        case rtc::LogLevel::Warning: Log::warn("[RTC] {}", message); break;
        case rtc::LogLevel::Info: Log::info("[RTC] {}", message); break;
        case rtc::LogLevel::Debug: Log::debug("[RTC] {}", message); break;
        case rtc::LogLevel::Verbose: Log::trace("[RTC] {}", message); break;
        default: Log::info("[RTC] {}", message); break;
    }
}


bool DataChannelClient::hasActiveConnection() const {
    for (const auto& [id, client] : m_clients) {
        if (client->dataChannel.has_value() && 
            client->dataChannel.value() && 
            client->dataChannel.value()->isOpen()) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> DataChannelClient::getConnectedPeers() const {
    std::vector<std::string> connected;
    for (const auto& [id, client] : m_clients) {
        if (client->dataChannel.has_value() && 
            client->dataChannel.value() && 
            client->dataChannel.value()->isOpen()) {
            connected.push_back(id);
        }
    }
    return connected;
}

bool DataChannelClient::isDataChannelOpen(const std::string& peerId) const {
    auto it = m_clients.find(peerId);
    if (it == m_clients.end()) return false;
    
    const auto& client = it->second;
    return client->dataChannel.has_value() && 
           client->dataChannel.value() && 
           client->dataChannel.value()->isOpen();
}







//---------------------------------------------------------


std::shared_ptr<ClientTrackData> DataChannelClient::addVideo(const std::shared_ptr<rtc::PeerConnection> pc, 
    const uint8_t payloadType, 
    const uint32_t ssrc, 
    const std::string cname, 
    const std::string msid, 
    const std::function<void (void)> onOpen) {
    auto video = rtc::Description::Video(cname, rtc::Description::Direction::SendOnly);
    video.addH264Codec(payloadType);
    video.addSSRC(ssrc, cname, msid, cname);
    auto track = pc->addTrack(video);
    // create RTP configuration
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, payloadType, rtc::H264RtpPacketizer::ClockRate);
    // create packetizer
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::Length, rtpConfig);
    // add RTCP SR handler
    auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // add RTCP NACK handler
    auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // set handler
    track->setMediaHandler(packetizer);
    track->onOpen(onOpen);
    auto trackData = std::make_shared<ClientTrackData>(track, srReporter);
    return trackData;
}






//----------------------------------------------------





uint32_t DataChannelClient::generateUniqueSSRC(const std::string& clientId) {
    // 方法 1: 基于客户端 ID 哈希
    std::hash<std::string> hasher;
    return hasher(clientId) & 0xFFFFFFFF;
    
    // 方法 2: 随机生成
    // std::random_device rd;
    // std::mt19937 gen(rd());
    // std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFE);
    // return dist(gen);
}

//---------------------------------------
std::shared_ptr<ClientTrackData> DataChannelClient::addAudio(const std::shared_ptr<rtc::PeerConnection> pc, 
    const uint8_t payloadType, 
    const uint32_t ssrc, 
    const std::string cname, 
    const std::string msid, 
    const std::function<void (void)> onOpen) {
    auto audio = rtc::Description::Audio(cname);
    audio.addOpusCodec(payloadType);
    audio.addSSRC(ssrc, cname, msid, cname);
    auto track = pc->addTrack(audio);
    // create RTP configuration
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, payloadType, rtc::OpusRtpPacketizer::DefaultClockRate);
    // create packetizer
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    // add RTCP SR handler
    auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // add RTCP NACK handler
    auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // set handler
    track->setMediaHandler(packetizer);
    track->onOpen(onOpen);
    auto trackData = std::make_shared<ClientTrackData>(track, srReporter);
    return trackData;
}
//---------------GStreamer--------------------------
// 前置声明：探针回调函数
static GstPadProbeReturn H264DataProbe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);


// ==========================================
// 探针回调：打印编码器数据（调试用）
// ==========================================
static GstPadProbeReturn H264DataProbe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);
        if (buffer) {
            // 打印：编码器正常输出H264数据
            gsize size = gst_buffer_get_size(buffer);
            g_print("[GStreamer] Probe: H264 Frame Size = %zu bytes\n", size);
        }
    }
    return GST_PAD_PROBE_OK;
}

void DataChannelClient::sendCachedDataToNewClient(std::shared_ptr<ClientTrackData> newClientTrack) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    Log::warn("sendCachedDataToNewClient CALLED!");
    // 注意：这里直接发送，不经过队列，因为是给特定客户端的
    try {
        if (!m_cachedSpsPps.empty()) {
            rtc::FrameInfo info(0);
            // 缓存的时间戳可以设为 0 或稍微往前一点
            newClientTrack->track->sendFrame(m_cachedSpsPps, info);
            Log::info("[DataChannelClient] Sent cached SPS/PPS to new client");
        }
        if (!m_cachedIdr.empty()) {
            // 计算 RTP 时间戳
            uint32_t rtp_ts = static_cast<uint32_t>(m_cachedIdrTs * 90 / 1000);
            rtc::FrameInfo info(rtp_ts);

            newClientTrack->track->sendFrame(m_cachedIdr, info);
            Log::info("[DataChannelClient] Sent cached IDR to new client");
        }
    } catch (const std::exception& e) {
        Log::error("[DataChannelClient] Failed to send cache: {}", e.what());
    }
}
