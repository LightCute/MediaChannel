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
    m_config.iceServers.push_back(turnServer);
    gst_init(nullptr, nullptr);
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

    videoTrackData->track->onOpen([this, peerId, client]() {
        Log::info("[DataChannelClient] Video track open for {}, starting GStreamer", peerId);
        // ✅ 现在client->video 100%有值，传给GST
        startGStreamerPipeline(client->video); 
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
    stopGStreamerPipeline(); // 先停媒体
    
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
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpConfig);
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



/// Start stream
void DataChannelClient::startStream() {
    std::shared_ptr<Stream> stream;
    if (m_avStream.has_value()) {
        stream = m_avStream.value();
        if (stream->isRunning) {
            // stream is already running
            return;
        }
    } else {
        stream = createStream("../../samples/h264", 30, "../../samples/opus");
        m_avStream = stream;
    }
    stream->start();
}

/// Add client to stream
/// @param client Client
/// @param adding_video True if adding video
void DataChannelClient::addToStream(std::shared_ptr<Client> client, bool isAddingVideo) {
    if (client->getState() == Client::State::Waiting) {
        client->setState(isAddingVideo ? Client::State::WaitingForAudio : Client::State::WaitingForVideo);
    } else if ((client->getState() == Client::State::WaitingForAudio && !isAddingVideo)
               || (client->getState() == Client::State::WaitingForVideo && isAddingVideo)) {

        // Audio and video tracks are collected now
        assert(client->video.has_value() && client->audio.has_value());
        auto video = client->video.value();

        if (m_avStream.has_value()) {
            sendInitialNalus(m_avStream.value(), video);
        }

        client->setState(Client::State::Ready);
    }
    if (client->getState() == Client::State::Ready) {
        startStream();
    }
}

// void DataChannelClient::addToStream(std::shared_ptr<Client> client, bool isAddingVideo) {
//     // -------------------------------------------------------------------------
//     // ✅ 修改点1：只要是添加视频，就直接进入 Ready 状态
//     // -------------------------------------------------------------------------
//     if (isAddingVideo) {
//         // 确保视频 Track 存在
//         assert(client->video.has_value());
//         auto video = client->video.value();

//         // 发送关键帧（如果流已创建）
//         if (m_avStream.has_value()) {
//             sendInitialNalus(m_avStream.value(), video);
//         }

//         // 直接设置为 Ready 状态
//         client->setState(Client::State::Ready);
//         Log::info("[DataChannelClient] Video track added, state set to Ready (audio optional)");
//     }

//     // -------------------------------------------------------------------------
//     // ✅ 修改点2：只要状态是 Ready，就启动流
//     // -------------------------------------------------------------------------
//     if (client->getState() == Client::State::Ready) {
//         startStream();
//     }
// }


/// Create stream
std::shared_ptr<Stream> DataChannelClient::createStream(
    const std::string h264Samples, 
    const unsigned fps, 
    const std::string opusSamples) {
    // video source
    auto video = std::make_shared<H264FileParser>(h264Samples, fps, true);
    // audio source
    auto audio = std::make_shared<OPUSFileParser>(opusSamples, true);

    auto stream = std::make_shared<Stream>(video, audio);
    // set callback responsible for sample sending
    stream->onSample([this, ws = make_weak_ptr(stream)](Stream::StreamSourceType type, uint64_t sampleTime, rtc::binary sample) {
        std::vector<ClientTrack> tracks{};
        std::string streamType = type == Stream::StreamSourceType::Video ? "video" : "audio";
        // get track for given type
        std::function<std::optional<std::shared_ptr<ClientTrackData>> (std::shared_ptr<Client>)> getTrackData = [type](std::shared_ptr<Client> client) {
            return type == Stream::StreamSourceType::Video ? client->video : client->audio;
        };
        // get all clients with Ready state
        for(auto id_client: m_clients) {
            auto id = id_client.first;
            auto client = id_client.second;
            auto optTrackData = getTrackData(client);
            if (client->getState() == Client::State::Ready && optTrackData.has_value()) {
                auto trackData = optTrackData.value();
                tracks.push_back(ClientTrack(id, trackData));
            }
        }
        if (!tracks.empty()) {
            for (auto clientTrack: tracks) {
                auto client = clientTrack.id;
                auto trackData = clientTrack.trackData;

                std::cout << "Sending " << streamType << " sample with size: " << std::to_string(sample.size()) << " to " << client << std::endl;
                try {
                    // send sample
                    trackData->track->sendFrame(sample, std::chrono::duration<double, std::micro>(sampleTime));
                } catch (const std::exception &e) {
                    std::cerr << "Unable to send "<< streamType << " packet: " << e.what() << std::endl;
                }
            }
        }
        m_MainThread.dispatch([this,ws]() {
            if (m_clients.empty()) {
                // we have no clients, stop the stream
                if (auto stream = ws.lock()) {
                    stream->stop();
                }
            }
        });
    });
    return stream;
}


/// Send previous key frame so browser can show something to user
/// @param stream Stream
/// @param video Video track data
void DataChannelClient::sendInitialNalus(std::shared_ptr<Stream> stream, std::shared_ptr<ClientTrackData> video) {
    auto h264 = dynamic_cast<H264FileParser *>(stream->video.get());
    auto initialNalus = h264->initialNALUS();

    // send previous NALU key frame so users don't have to wait to see stream works
    if (!initialNalus.empty()) {
        const double frameDuration_s = double(h264->getSampleDuration_us()) / (1000 * 1000);
        const uint32_t frameTimestampDuration = video->sender->rtpConfig->secondsToTimestamp(frameDuration_s);
        video->sender->rtpConfig->timestamp = video->sender->rtpConfig->startTimestamp - frameTimestampDuration * 2;
        video->track->send(initialNalus);
        video->sender->rtpConfig->timestamp += frameTimestampDuration;
        // Send initial NAL units again to start stream in firefox browser
        video->track->send(initialNalus);
    }
}


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

// --- 启动 Linux GStreamer 实时采集 Pipeline ---
void DataChannelClient::startGStreamerPipeline(std::optional<std::shared_ptr<ClientTrackData>> videoTrack) {
    if (m_gstRunning.exchange(true)) {
        Log::warn("[GStreamer] Pipeline already running");
        return;
    }

    if (videoTrack.has_value()) {
        // 2. 取出内部的 shared_ptr 直接赋值！
        m_currentVideoTrack = videoTrack.value(); 
    }    
    m_firstPts = 0;

    Log::info("[GStreamer] Creating pipeline...");

    // ✅ 修复1：给关键元素命名 name=xxx （必须！）
    // ✅ 修复2：简化管道，去掉JPEG解码（兼容性更强，绝大多数摄像头通用）
    // const char* pipelineDesc = R"(
    //     v4l2src device=/dev/video0 name=video_src ! 
    //     video/x-raw,width=640,height=480,framerate=30/1 ! 
    //     queue leaky=downstream max-size-buffers=1 ! 
    //     x264enc name=h264_encoder tune=zerolatency speed-preset=ultrafast bitrate=1000 key-int-max=30 ! 
    //     video/x-h264,stream-format=byte-stream,alignment=au ! 
    //     appsink name=video_sink emit-signals=true sync=false
    // )";

    const char* pipelineDesc = R"(
        v4l2src device=/dev/video0 name=video_src ! 
        image/jpeg,width=640,height=480,framerate=30/1 ! 
        jpegdec ! 
        videoconvert ! 
        queue leaky=downstream max-size-buffers=1 ! 
        x264enc name=h264_encoder tune=zerolatency speed-preset=ultrafast bitrate=1000 key-int-max=30 ! 
        video/x-h264,stream-format=byte-stream,alignment=au ! 
        appsink name=video_sink emit-signals=true sync=false
    )";

    GError* error = nullptr;
    m_gstPipeline = gst_parse_launch(pipelineDesc, &error);
    if (error) {
        Log::error("[GStreamer] Failed to parse pipeline: {}", error->message);
        g_error_free(error);
        m_gstRunning = false;
        return;
    }
    Log::info("[GStreamer] Pipeline parsed successfully.");

    // ==========================================
    // ✅ 修复2：用正确的name获取元素（和pipeline里的name一致）
    // ==========================================
    GstElement* src = gst_bin_get_by_name(GST_BIN(m_gstPipeline), "video_src");
    GstElement* encoder = gst_bin_get_by_name(GST_BIN(m_gstPipeline), "h264_encoder");
    m_gstAppSink = gst_bin_get_by_name(GST_BIN(m_gstPipeline), "video_sink");

    if (!src || !encoder || !m_gstAppSink) {
        Log::error("[GStreamer] Failed to find elements! src={}, encoder={}, appsink={}", 
                  (src!=nullptr), (encoder!=nullptr), (m_gstAppSink!=nullptr));
        if (src) gst_object_unref(src);
        if (encoder) gst_object_unref(encoder);
        gst_object_unref(m_gstPipeline);
        m_gstPipeline = nullptr;
        m_gstRunning = false;
        return;
    }
    Log::info("[GStreamer] All elements found (src, encoder, sink).");

    // ==========================================
    // 数据探针（监控编码器输出）
    // ==========================================
    GstPad* encoder_src_pad = gst_element_get_static_pad(encoder, "src");
    if (encoder_src_pad) {
        gst_pad_add_probe(encoder_src_pad, GST_PAD_PROBE_TYPE_BUFFER, 
            H264DataProbe, this, nullptr);
        gst_object_unref(encoder_src_pad);
        Log::info("[GStreamer] Probe attached to encoder output.");
    }

    // ==========================================
    // GStreamer 总线错误监听（核心调试日志）
    // ==========================================
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_gstPipeline));
    gst_bus_add_watch(bus, [](GstBus* bus, GstMessage* msg, gpointer user_data) -> gboolean {
        auto* self = static_cast<DataChannelClient*>(user_data);
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err;
                gchar* debug;
                gst_message_parse_error(msg, &err, &debug);
                Log::error("[GStreamer] BUS ERROR: {} | Debug: {}", err->message, debug ? debug : "none");
                g_error_free(err);
                g_free(debug);
                break;
            }
            case GST_MESSAGE_WARNING: {
                GError* err;
                gchar* debug;
                gst_message_parse_warning(msg, &err, &debug);
                Log::warn("[GStreamer] BUS WARNING: {}", err->message);
                g_error_free(err);
                g_free(debug);
                break;
            }
            case GST_MESSAGE_EOS:
                Log::info("[GStreamer] BUS: End of Stream");
                break;
            case GST_MESSAGE_STATE_CHANGED:
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->m_gstPipeline)) {
                    GstState old_s, new_s, pending;
                    gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
                    Log::info("[GStreamer] State: {} -> {}", 
                              gst_element_state_get_name(old_s), gst_element_state_get_name(new_s));
                }
                break;
            default:
                break;
        }
        return TRUE;
    }, this);
    gst_object_unref(bus);

    // ==========================================
    // 配置 appsink 回调
    // ==========================================
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_gstAppSink), &callbacks, this, nullptr);
    Log::info("[GStreamer] Appsink callback set.");

    // ==========================================
    // 启动管道
    // ==========================================
    GstStateChangeReturn ret = gst_element_set_state(m_gstPipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        Log::error("[GStreamer] Failed to start PLAYING state!");
        gst_object_unref(src);
        gst_object_unref(encoder);
        gst_object_unref(m_gstAppSink);
        gst_object_unref(m_gstPipeline);
        m_gstPipeline = m_gstAppSink = nullptr;
        m_gstRunning = false;
        return;
    }

    // 释放临时引用
    gst_object_unref(src);
    gst_object_unref(encoder);

    Log::info("[GStreamer] ✅ Pipeline STARTED SUCCESSFULLY! Waiting for camera data...");
}

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

// ==========================================
// appsink 核心回调：获取H264数据发给WebRTC
// ==========================================
GstFlowReturn DataChannelClient::onNewSample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<DataChannelClient*>(user_data);
    
    // 调试：证明回调触发
    g_print("[GStreamer] onNewSample Triggered!\n");

    if (!self->m_gstRunning || !self->m_currentVideoTrack) {
        return GST_FLOW_OK;
    }

    // 取数据
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // 转WebRTC数据格式
    rtc::binary frame(reinterpret_cast<std::byte*>(map.data), 
                     reinterpret_cast<std::byte*>(map.data + map.size));

    try {
        auto& track = self->m_currentVideoTrack;
        const uint32_t clock = track->sender->rtpConfig->clockRate;
        const uint32_t ts_inc = clock / 30; // 30fps

        // 时间戳管理
        if (self->m_firstPts == 0) {
            track->sender->rtpConfig->timestamp = track->sender->rtpConfig->startTimestamp;
            self->m_firstPts = 1;
        } else {
            track->sender->rtpConfig->timestamp += ts_inc;
        }

        // 发送帧
        track->track->sendFrame(frame, rtc::FrameInfo(track->sender->rtpConfig->timestamp));
        g_print("[GStreamer] Sent Frame: %zu bytes\n", map.size);
    } 
    catch (const std::exception& e) {
        Log::error("[GStreamer] Send Failed: {}", e.what());
    }

    // 释放资源
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// ==========================================
// 停止管道
// ==========================================
void DataChannelClient::stopGStreamerPipeline() {
    if (!m_gstRunning.exchange(false)) return;
    Log::info("[GStreamer] Stopping...");

    if (m_gstPipeline) {
        gst_element_set_state(m_gstPipeline, GST_STATE_NULL);
        gst_object_unref(m_gstPipeline);
        m_gstPipeline = nullptr;
    }
    m_gstAppSink = nullptr;
    m_currentVideoTrack.reset();
}