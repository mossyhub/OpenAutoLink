/*
 * Headless TCP server using opencardev-aasdk (AAP Proto 1.6)
 * Tests whether the v1.6 protocol with new ServiceConfiguration format
 * gets the phone past ServiceDiscovery into ChannelOpen.
 */

#include <iostream>
#include <thread>
#include <vector>
#include <boost/asio.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Transport/TCPTransport.hpp>
#include <aasdk/TCP/TCPEndpoint.hpp>
#include <aasdk/TCP/TCPWrapper.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>

// New proto includes
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h>
#include <aap_protobuf/service/control/message/HeadUnitInfo.pb.h>
#include <aap_protobuf/service/control/message/ConnectionConfiguration.pb.h>
#include <aap_protobuf/service/control/message/PingConfiguration.pb.h>
#include <aap_protobuf/service/control/message/DriverPosition.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusRequest.pb.h>
#include <aap_protobuf/service/control/message/ByeByeRequest.pb.h>
#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>
#include <aap_protobuf/service/control/message/NavFocusRequestNotification.pb.h>
#include <aap_protobuf/service/control/message/PingRequest.pb.h>
#include <aap_protobuf/service/control/message/PingResponse.pb.h>
#include <aap_protobuf/service/control/message/VoiceSessionNotification.pb.h>
#include <aap_protobuf/service/control/message/BatteryStatusNotification.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/NavFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/AuthResponse.pb.h>
#include <aap_protobuf/service/Service.pb.h>
#include <aap_protobuf/service/media/sink/MediaSinkService.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoConfiguration.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoCodecResolutionType.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoFrameRateType.pb.h>
#include <aap_protobuf/service/media/sink/message/AudioStreamType.pb.h>
#include <aap_protobuf/service/media/shared/message/AudioConfiguration.pb.h>
#include <aap_protobuf/service/media/shared/message/MediaCodecType.pb.h>
#include <aap_protobuf/service/sensorsource/SensorSourceService.pb.h>
#include <aap_protobuf/service/sensorsource/message/Sensor.pb.h>
#include <aap_protobuf/service/inputsource/InputSourceService.pb.h>
#include <aap_protobuf/service/bluetooth/BluetoothService.pb.h>
#include <aap_protobuf/shared/MessageStatus.pb.h>

using boost::asio::ip::tcp;

class HeadlessEntity
    : public aasdk::channel::control::IControlServiceChannelEventHandler
    , public std::enable_shared_from_this<HeadlessEntity>
{
public:
    HeadlessEntity(boost::asio::io_service& ios,
                   aasdk::messenger::ICryptor::Pointer cryptor,
                   aasdk::transport::ITransport::Pointer transport,
                   aasdk::messenger::IMessenger::Pointer messenger)
        : strand_(ios)
        , cryptor_(std::move(cryptor))
        , transport_(std::move(transport))
        , messenger_(std::move(messenger))
        , controlChannel_(std::make_shared<aasdk::channel::control::ControlServiceChannel>(strand_, messenger_))
        , pingTimer_(ios)
    {}

    void start() {
        strand_.dispatch([this, self = shared_from_this()]() {
            std::cout << "[Entity] Starting, sending VersionRequest..." << std::endl;

            schedulePing();

            auto promise = aasdk::channel::SendPromise::defer(strand_);
            promise->then([](){
                std::cout << "[Entity] VersionRequest sent OK" << std::endl;
            }, [this](auto e){ onChannelError(e); });
            controlChannel_->sendVersionRequest(std::move(promise));
            controlChannel_->receive(shared_from_this());
        });
    }

    // ---- Control channel events ----
    void onVersionResponse(uint16_t major, uint16_t minor,
                          aap_protobuf::shared::MessageStatus status) override
    {
        std::cout << "[Entity] VersionResponse v" << major << "." << minor
                  << " status=" << static_cast<int>(status) << std::endl;

        if (status != aap_protobuf::shared::STATUS_SUCCESS) {
            std::cout << "[Entity] VERSION MISMATCH" << std::endl;
            return;
        }

        try {
            cryptor_->doHandshake();
            auto promise = aasdk::channel::SendPromise::defer(strand_);
            promise->then([](){
                std::cout << "[Entity] Handshake sent OK" << std::endl;
            }, [this](auto e){ onChannelError(e); });
            controlChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(promise));
            controlChannel_->receive(shared_from_this());
        } catch (const aasdk::error::Error& e) {
            std::cout << "[Entity] Handshake init error: " << e.what() << std::endl;
        }
    }

    void onHandshake(const aasdk::common::DataConstBuffer& payload) override {
        std::cout << "[Entity] Handshake received, size=" << payload.size << std::endl;
        try {
            cryptor_->writeHandshakeBuffer(payload);
            if (!cryptor_->doHandshake()) {
                std::cout << "[Entity] Handshake continue..." << std::endl;
                auto promise = aasdk::channel::SendPromise::defer(strand_);
                promise->then([](){}, [this](auto e){ onChannelError(e); });
                controlChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(promise));
            } else {
                std::cout << "[Entity] *** AUTH COMPLETE ***" << std::endl;
                aap_protobuf::service::control::message::AuthResponse auth;
                auth.set_status(aap_protobuf::shared::STATUS_SUCCESS);
                auto promise = aasdk::channel::SendPromise::defer(strand_);
                promise->then([](){
                    std::cout << "[Entity] AuthComplete sent OK" << std::endl;
                }, [this](auto e){ onChannelError(e); });
                controlChannel_->sendAuthComplete(auth, std::move(promise));
            }
            controlChannel_->receive(shared_from_this());
        } catch (const aasdk::error::Error& e) {
            std::cout << "[Entity] Handshake error: " << e.what() << std::endl;
        }
    }

    void onServiceDiscoveryRequest(
        const aap_protobuf::service::control::message::ServiceDiscoveryRequest& request) override
    {
        std::cout << "[Entity] *** ServiceDiscoveryRequest ***" << std::endl;
        std::cout << "[Entity]   device: " << request.device_name()
                  << " / " << request.label_text() << std::endl;

        aap_protobuf::service::control::message::ServiceDiscoveryResponse response;
        response.mutable_channels()->Reserve(256);

        // v1.6 fields
        response.set_driver_position(
            aap_protobuf::service::control::message::DRIVER_POSITION_LEFT);
        response.set_display_name("OpenAuto");
        response.set_probe_for_support(false);

        // Connection configuration
        auto* connConfig = response.mutable_connection_configuration();
        auto* pingConfig = connConfig->mutable_ping_configuration();
        pingConfig->set_timeout_ms(5000);
        pingConfig->set_interval_ms(1500);
        pingConfig->set_high_latency_threshold_ms(500);
        pingConfig->set_tracked_ping_count(5);

        // HeadUnit info
        auto* huInfo = response.mutable_headunit_info();
        huInfo->set_make("OpenAuto");
        huInfo->set_model("Universal");
        huInfo->set_year("2024");
        huInfo->set_vehicle_id("piaa-001");
        huInfo->set_head_unit_make("PiAA");
        huInfo->set_head_unit_model("Headless Bridge");
        huInfo->set_head_unit_software_build("1");
        huInfo->set_head_unit_software_version("1.0");

        // ---- Video service (MediaSinkService: VIDEO) ----
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO));
            auto* ms = svc->mutable_media_sink_service();
            ms->set_available_type(
                aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
            ms->set_available_while_in_call(true);
            auto* vc = ms->add_video_configs();
            vc->set_codec_resolution(
                aap_protobuf::service::media::sink::message::VIDEO_RESOLUTION_1080P);
            vc->set_frame_rate(
                aap_protobuf::service::media::sink::message::VIDEO_FPS_60);
            vc->set_density(160);
            vc->set_height_margin(0);
            vc->set_width_margin(0);
        }

        // ---- Media Audio (MediaSinkService: AUDIO MEDIA) ----
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO));
            auto* ms = svc->mutable_media_sink_service();
            ms->set_available_type(
                aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
            ms->set_audio_type(
                aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA);
            auto* ac = ms->add_audio_configs();
            ac->set_sample_rate(48000);
            ac->set_number_of_bits(16);
            ac->set_number_of_channels(2);
        }

        // ---- Speech Audio ----
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO));
            auto* ms = svc->mutable_media_sink_service();
            ms->set_available_type(
                aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
            ms->set_audio_type(
                aap_protobuf::service::media::sink::message::AUDIO_STREAM_GUIDANCE);
            auto* ac = ms->add_audio_configs();
            ac->set_sample_rate(16000);
            ac->set_number_of_bits(16);
            ac->set_number_of_channels(1);
        }

        // ---- System Audio ----
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO));
            auto* ms = svc->mutable_media_sink_service();
            ms->set_available_type(
                aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
            ms->set_audio_type(
                aap_protobuf::service::media::sink::message::AUDIO_STREAM_SYSTEM);
            auto* ac = ms->add_audio_configs();
            ac->set_sample_rate(16000);
            ac->set_number_of_bits(16);
            ac->set_number_of_channels(1);
        }

        // ---- Audio Input (MediaSourceService: microphone) ----
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE));
            auto* msrc = svc->mutable_media_source_service();
            auto* ac = msrc->mutable_audio_config();
            ac->set_sample_rate(16000);
            ac->set_number_of_bits(16);
            ac->set_number_of_channels(1);
        }

        // ---- Sensor ----
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::SENSOR));
            auto* ss = svc->mutable_sensor_source_service();
            auto* sensor = ss->add_sensors();
            sensor->set_type(
                aap_protobuf::service::sensorsource::message::SENSOR_TYPE_DRIVING_STATUS);
        }

        // ---- Input ----
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::INPUT_SOURCE));
            auto* is = svc->mutable_input_source_service();
            auto* ts = is->add_touchscreen();
            ts->set_width(1920);
            ts->set_height(1080);
        }

        // ---- Bluetooth ----
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::BLUETOOTH));
            auto* bs = svc->mutable_bluetooth_service();
            bs->set_car_address("CC:8D:A2:0C:5C:EA");
        }

        std::cout << "[Entity] ServiceDiscoveryResponse: " << response.channels_size()
                  << " channels, " << response.ByteSizeLong() << " bytes" << std::endl;

        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){
            std::cout << "[Entity] *** ServiceDiscoveryResponse sent OK ***" << std::endl;
        }, [this](auto e){ onChannelError(e); });
        controlChannel_->sendServiceDiscoveryResponse(response, std::move(promise));
        controlChannel_->receive(shared_from_this());
    }

    void onAudioFocusRequest(
        const aap_protobuf::service::control::message::AudioFocusRequest& request) override
    {
        std::cout << "[Entity] *** AudioFocusRequest ***" << std::endl;
        aap_protobuf::service::control::message::AudioFocusNotification resp;
        resp.set_audio_focus_state(
            aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN);
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        controlChannel_->sendAudioFocusResponse(resp, std::move(promise));
        controlChannel_->receive(shared_from_this());
    }

    void onByeByeRequest(const aap_protobuf::service::control::message::ByeByeRequest& request) override {
        std::cout << "[Entity] ByeByeRequest" << std::endl;
        aap_protobuf::service::control::message::ByeByeResponse resp;
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        controlChannel_->sendByeByeResponse(resp, std::move(promise));
    }

    void onByeByeResponse(const aap_protobuf::service::control::message::ByeByeResponse&) override {
        std::cout << "[Entity] ByeByeResponse" << std::endl;
    }

    void onBatteryStatusNotification(
        const aap_protobuf::service::control::message::BatteryStatusNotification& n) override {
        std::cout << "[Entity] BatteryStatus" << std::endl;
        controlChannel_->receive(shared_from_this());
    }

    void onNavigationFocusRequest(
        const aap_protobuf::service::control::message::NavFocusRequestNotification& request) override {
        std::cout << "[Entity] *** NavFocusRequest ***" << std::endl;
        aap_protobuf::service::control::message::NavFocusNotification resp;
        resp.set_focus_type(aap_protobuf::service::control::message::NAV_FOCUS_PROJECTED);
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        controlChannel_->sendNavigationFocusResponse(resp, std::move(promise));
        controlChannel_->receive(shared_from_this());
    }

    void onVoiceSessionRequest(
        const aap_protobuf::service::control::message::VoiceSessionNotification& request) override {
        std::cout << "[Entity] VoiceSession" << std::endl;
        controlChannel_->receive(shared_from_this());
    }

    void onPingRequest(const aap_protobuf::service::control::message::PingRequest& request) override {
        std::cout << "[Entity] PingRequest -> sending PingResponse" << std::endl;
        aap_protobuf::service::control::message::PingResponse resp;
        resp.set_data(request.data());
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        controlChannel_->sendPingResponse(resp, std::move(promise));
        controlChannel_->receive(shared_from_this());
    }

    void onPingResponse(const aap_protobuf::service::control::message::PingResponse&) override {
        std::cout << "[Entity] PingResponse" << std::endl;
        controlChannel_->receive(shared_from_this());
    }

    void onChannelError(const aasdk::error::Error& e) override {
        std::cout << "[Entity] *** CHANNEL ERROR: " << e.what() << " ***" << std::endl;
    }

private:
    void schedulePing() {
        pingTimer_.expires_from_now(boost::posix_time::milliseconds(5000));
        pingTimer_.async_wait([this, self = shared_from_this()](const boost::system::error_code& ec) {
            if (!ec) {
                sendPing();
                schedulePing();
            }
        });
    }

    void sendPing() {
        aap_protobuf::service::control::message::PingRequest req;
        req.set_data(42);
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        controlChannel_->sendPingRequest(req, std::move(promise));
    }

    boost::asio::io_service::strand strand_;
    aasdk::messenger::ICryptor::Pointer cryptor_;
    aasdk::transport::ITransport::Pointer transport_;
    aasdk::messenger::IMessenger::Pointer messenger_;
    aasdk::channel::control::ControlServiceChannel::Pointer controlChannel_;
    boost::asio::deadline_timer pingTimer_;
};

int main()
{
    std::cout << "=== Headless OCD-AASDK v1.6 TCP Server ===" << std::endl;
    std::cout << std::flush;

    boost::asio::io_service ioService;
    boost::asio::io_service::work work(ioService);

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&ioService]() { ioService.run(); });

    tcp::acceptor acceptor(ioService, tcp::endpoint(tcp::v4(), 5277));
    acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    std::cout << "Listening on :5277" << std::endl;

    while (true) {
        auto socket = std::make_shared<tcp::socket>(ioService);
        boost::system::error_code ec;
        acceptor.accept(*socket, ec);
        if (ec) { std::cout << "Accept error: " << ec.message() << std::endl; continue; }

        auto remote = socket->remote_endpoint();
        std::cout << "\n=== Connection from " << remote.address().to_string()
                  << ":" << remote.port() << " ===" << std::endl;

        aasdk::tcp::TCPWrapper tcpWrapper;
        auto tcpEndpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(tcpWrapper, std::move(socket));
        auto transport = std::make_shared<aasdk::transport::TCPTransport>(ioService, std::move(tcpEndpoint));

        auto sslWrapper = std::make_shared<aasdk::transport::SSLWrapper>();
        auto cryptor = std::make_shared<aasdk::messenger::Cryptor>(std::move(sslWrapper));
        cryptor->init();

        auto messenger = std::make_shared<aasdk::messenger::Messenger>(
            ioService,
            std::make_shared<aasdk::messenger::MessageInStream>(ioService, transport, cryptor),
            std::make_shared<aasdk::messenger::MessageOutStream>(ioService, transport, cryptor));

        auto entity = std::make_shared<HeadlessEntity>(
            ioService, std::move(cryptor), std::move(transport), std::move(messenger));
        entity->start();
    }

    for (auto& t : threads) t.join();
    return 0;
}
