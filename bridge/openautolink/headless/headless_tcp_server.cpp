/*
 * Headless OpenAuto TCP server for wireless Android Auto testing.
 * Accepts a phone connection on port 5277 and runs the full OpenAuto
 * AndroidAutoEntity protocol stack without a display.
 *
 * Build:
 *   cd /tmp/openauto-headless-test
 *   cmake /tmp/openauto-headless-test -DCMAKE_BUILD_TYPE=Release \
 *     -DAASDK_INCLUDE_DIRS=/opt/pi-aa/external/aasdk/include \
 *     -DAASDK_LIBRARIES=/opt/pi-aa/external/aasdk/lib/libaasdk.so \
 *     -DAASDK_PROTO_INCLUDE_DIRS=/tmp/aasdk-build \
 *     -DAASDK_PROTO_LIBRARIES=/opt/pi-aa/external/aasdk/lib/libaasdk_proto.so
 *   make -j4
 */

#include <iostream>
#include <thread>
#include <vector>
#include <boost/asio.hpp>
#include <f1x/aasdk/Transport/SSLWrapper.hpp>
#include <f1x/aasdk/Transport/TCPTransport.hpp>
#include <f1x/aasdk/TCP/TCPEndpoint.hpp>
#include <f1x/aasdk/TCP/TCPWrapper.hpp>
#include <f1x/aasdk/Messenger/Cryptor.hpp>
#include <f1x/aasdk/Messenger/MessageInStream.hpp>
#include <f1x/aasdk/Messenger/MessageOutStream.hpp>
#include <f1x/aasdk/Messenger/Messenger.hpp>
#include <f1x/aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <f1x/aasdk/Channel/AV/VideoServiceChannel.hpp>
#include <f1x/aasdk/Channel/AV/AudioServiceChannel.hpp>
#include <f1x/aasdk/Channel/AV/IVideoServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Channel/AV/IAudioServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Channel/Input/InputServiceChannel.hpp>
#include <f1x/aasdk/Channel/Input/IInputServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Channel/Sensor/SensorServiceChannel.hpp>
#include <f1x/aasdk/Channel/Sensor/ISensorServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Channel/Bluetooth/BluetoothServiceChannel.hpp>
#include <f1x/aasdk/Channel/Bluetooth/IBluetoothServiceChannelEventHandler.hpp>
#include <aasdk_proto/ControlMessageIdsEnum.pb.h>
#include <aasdk_proto/ServiceDiscoveryResponseMessage.pb.h>
#include <aasdk_proto/ServiceDiscoveryRequestMessage.pb.h>
#include <aasdk_proto/ChannelDescriptorData.pb.h>
#include <aasdk_proto/VideoFPSEnum.pb.h>
#include <aasdk_proto/VideoResolutionEnum.pb.h>
#include <aasdk_proto/AVChannelSetupResponseMessage.pb.h>
#include <aasdk_proto/AVChannelStartIndicationMessage.pb.h>
#include <aasdk_proto/AVChannelStopIndicationMessage.pb.h>
#include <aasdk_proto/AVInputOpenResponseMessage.pb.h>
#include <aasdk_proto/AudioTypeEnum.pb.h>
#include <aasdk_proto/AVStreamTypeEnum.pb.h>
#include <aasdk_proto/ChannelOpenResponseMessage.pb.h>
#include <aasdk_proto/PingRequestMessage.pb.h>
#include <aasdk_proto/PingResponseMessage.pb.h>
#include <aasdk_proto/ShutdownRequestMessage.pb.h>
#include <aasdk_proto/ShutdownResponseMessage.pb.h>
#include <aasdk_proto/NavigationFocusRequestMessage.pb.h>
#include <aasdk_proto/NavigationFocusResponseMessage.pb.h>
#include <aasdk_proto/AudioFocusRequestMessage.pb.h>
#include <aasdk_proto/AudioFocusResponseMessage.pb.h>
#include <aasdk_proto/AuthCompleteIndicationMessage.pb.h>
#include <aasdk_proto/VideoFocusIndicationMessage.pb.h>
#include <aasdk_proto/InputEventIndicationMessage.pb.h>
#include <aasdk_proto/SensorEventIndicationMessage.pb.h>
#include <aasdk_proto/SensorStartRequestMessage.pb.h>
#include <aasdk_proto/BluetoothPairingRequestMessage.pb.h>
#include <aasdk_proto/BluetoothPairingResponseMessage.pb.h>

namespace f1x { namespace aasdk { namespace channel { namespace control {
    class IControlServiceChannelEventHandler;
}}}}

using boost::asio::ip::tcp;

// ---------- Minimal IService interface (from OpenAuto) ----------
class IService {
public:
    virtual ~IService() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) = 0;
};

// ---------- Stub Video Service ----------
class StubVideoService
    : public IService
    , public f1x::aasdk::channel::av::IVideoServiceChannelEventHandler
    , public std::enable_shared_from_this<StubVideoService>
{
public:
    StubVideoService(boost::asio::io_service& ios, f1x::aasdk::messenger::IMessenger::Pointer messenger)
        : strand_(ios)
        , channel_(std::make_shared<f1x::aasdk::channel::av::VideoServiceChannel>(strand_, std::move(messenger)))
    {}

    void start() override {
        std::cout << "[VideoService] start" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void stop() override { std::cout << "[VideoService] stop" << std::endl; }

    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        auto* vi = ch->mutable_av_channel();
        vi->set_stream_type(f1x::aasdk::proto::enums::AVStreamType::VIDEO);
        vi->set_available_while_in_call(true);

        auto* vc = vi->add_video_configs();
        vc->set_video_resolution(f1x::aasdk::proto::enums::VideoResolution::_1080p);
        vc->set_video_fps(f1x::aasdk::proto::enums::VideoFPS::_60);
        vc->set_density(160);
        vc->set_margin_height(0);
        vc->set_margin_width(0);
    }

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest& request) override {
        std::cout << "[VideoService] ChannelOpenRequest priority=" << request.priority() << std::endl;
        f1x::aasdk::proto::messages::ChannelOpenResponse response;
        response.set_status(f1x::aasdk::proto::enums::Status::OK);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendChannelOpenResponse(response, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onAVChannelSetupRequest(const f1x::aasdk::proto::messages::AVChannelSetupRequest& request) override {
        std::cout << "[VideoService] AVChannelSetupRequest config=" << request.config_index() << std::endl;
        f1x::aasdk::proto::messages::AVChannelSetupResponse response;
        response.set_media_status(f1x::aasdk::proto::enums::AVChannelSetupStatus::OK);
        response.set_max_unacked(1);
        response.set_configs_count(1);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendAVChannelSetupResponse(response, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onAVChannelStartIndication(const f1x::aasdk::proto::messages::AVChannelStartIndication& indication) override {
        std::cout << "[VideoService] AVChannelStartIndication session=" << indication.session() << std::endl;
        // Send video focus
        f1x::aasdk::proto::messages::VideoFocusIndication focus;
        focus.set_focus_mode(f1x::aasdk::proto::enums::VideoFocusMode::FOCUSED);
        focus.set_unrequested(false);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendVideoFocusIndication(focus, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onAVChannelStopIndication(const f1x::aasdk::proto::messages::AVChannelStopIndication&) override {
        std::cout << "[VideoService] AVChannelStopIndication" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void onAVMediaWithTimestampIndication(f1x::aasdk::messenger::Timestamp::ValueType ts, const f1x::aasdk::common::DataConstBuffer& buf) override {
        std::cout << "[VideoService] VideoData ts=" << ts << " size=" << buf.size << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void onAVMediaIndication(const f1x::aasdk::common::DataConstBuffer& buf) override {
        std::cout << "[VideoService] VideoData size=" << buf.size << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void onVideoFocusRequest(const f1x::aasdk::proto::messages::VideoFocusRequest& req) override {
        std::cout << "[VideoService] VideoFocusRequest mode=" << req.focus_mode() << std::endl;
        f1x::aasdk::proto::messages::VideoFocusIndication focus;
        focus.set_focus_mode(f1x::aasdk::proto::enums::VideoFocusMode::FOCUSED);
        focus.set_unrequested(false);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendVideoFocusIndication(focus, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onChannelError(const f1x::aasdk::error::Error& e) override {
        std::cout << "[VideoService] error: " << e.what() << std::endl;
    }

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::av::VideoServiceChannel::Pointer channel_;
};

// ---------- Stub Audio Service ----------
class StubAudioService
    : public IService
    , public f1x::aasdk::channel::av::IAudioServiceChannelEventHandler
    , public std::enable_shared_from_this<StubAudioService>
{
public:
    StubAudioService(boost::asio::io_service& ios, f1x::aasdk::messenger::IMessenger::Pointer messenger,
                     f1x::aasdk::messenger::ChannelId channelId, const std::string& name,
                     f1x::aasdk::proto::enums::AudioType::Enum audioType,
                     uint32_t sampleRate, uint32_t bitDepth, uint32_t channels)
        : strand_(ios)
        , channel_(std::make_shared<f1x::aasdk::channel::av::AudioServiceChannel>(strand_, std::move(messenger), channelId))
        , name_(name), audioType_(audioType), sampleRate_(sampleRate), bitDepth_(bitDepth), channelCount_(channels)
    {}

    void start() override {
        std::cout << "[" << name_ << "] start" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void stop() override { std::cout << "[" << name_ << "] stop" << std::endl; }

    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        auto* ac = ch->mutable_av_channel();
        ac->set_stream_type(f1x::aasdk::proto::enums::AVStreamType::AUDIO);
        ac->set_audio_type(audioType_);
        auto* cfg = ac->add_audio_configs();
        cfg->set_sample_rate(sampleRate_);
        cfg->set_bit_depth(bitDepth_);
        cfg->set_channel_count(channelCount_);
    }

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest&) override {
        std::cout << "[" << name_ << "] ChannelOpenRequest" << std::endl;
        f1x::aasdk::proto::messages::ChannelOpenResponse response;
        response.set_status(f1x::aasdk::proto::enums::Status::OK);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendChannelOpenResponse(response, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onAVChannelSetupRequest(const f1x::aasdk::proto::messages::AVChannelSetupRequest& req) override {
        std::cout << "[" << name_ << "] AVChannelSetupRequest config=" << req.config_index() << std::endl;
        f1x::aasdk::proto::messages::AVChannelSetupResponse response;
        response.set_media_status(f1x::aasdk::proto::enums::AVChannelSetupStatus::OK);
        response.set_max_unacked(1);
        response.set_configs_count(1);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendAVChannelSetupResponse(response, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onAVChannelStartIndication(const f1x::aasdk::proto::messages::AVChannelStartIndication& ind) override {
        std::cout << "[" << name_ << "] AVChannelStartIndication session=" << ind.session() << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void onAVChannelStopIndication(const f1x::aasdk::proto::messages::AVChannelStopIndication&) override {
        std::cout << "[" << name_ << "] AVChannelStopIndication" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void onAVMediaWithTimestampIndication(f1x::aasdk::messenger::Timestamp::ValueType ts, const f1x::aasdk::common::DataConstBuffer& buf) override {
        std::cout << "[" << name_ << "] AudioData ts=" << ts << " size=" << buf.size << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void onAVMediaIndication(const f1x::aasdk::common::DataConstBuffer& buf) override {
        std::cout << "[" << name_ << "] AudioData size=" << buf.size << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void onChannelError(const f1x::aasdk::error::Error& e) override {
        std::cout << "[" << name_ << "] error: " << e.what() << std::endl;
    }

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::av::AudioServiceChannel::Pointer channel_;
    std::string name_;
    f1x::aasdk::proto::enums::AudioType::Enum audioType_;
    uint32_t sampleRate_, bitDepth_, channelCount_;
};

// ---------- Stub Audio Input Service ----------
class StubAudioInputService
    : public IService
    , public f1x::aasdk::channel::av::IAudioServiceChannelEventHandler
    , public std::enable_shared_from_this<StubAudioInputService>
{
public:
    StubAudioInputService(boost::asio::io_service& ios, f1x::aasdk::messenger::IMessenger::Pointer messenger)
        : strand_(ios)
        , channel_(std::make_shared<f1x::aasdk::channel::av::AudioServiceChannel>(strand_, std::move(messenger),
                   f1x::aasdk::messenger::ChannelId::AV_INPUT))
    {}

    void start() override {
        std::cout << "[AudioInput] start" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void stop() override {}

    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        auto* av = ch->mutable_av_input_channel();
        auto* cfg = av->add_audio_configs();
        cfg->set_sample_rate(16000);
        cfg->set_bit_depth(16);
        cfg->set_channel_count(1);
    }

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest&) override {
        std::cout << "[AudioInput] ChannelOpenRequest" << std::endl;
        f1x::aasdk::proto::messages::ChannelOpenResponse resp;
        resp.set_status(f1x::aasdk::proto::enums::Status::OK);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendChannelOpenResponse(resp, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onAVChannelSetupRequest(const f1x::aasdk::proto::messages::AVChannelSetupRequest&) override {
        std::cout << "[AudioInput] AVChannelSetupRequest" << std::endl;
        f1x::aasdk::proto::messages::AVInputOpenResponse resp;
        resp.set_session(1);
        resp.set_value(0);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendAVInputOpenResponse(resp, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onAVChannelStartIndication(const f1x::aasdk::proto::messages::AVChannelStartIndication&) override {
        channel_->receive(this->shared_from_this());
    }
    void onAVChannelStopIndication(const f1x::aasdk::proto::messages::AVChannelStopIndication&) override {
        channel_->receive(this->shared_from_this());
    }
    void onAVMediaWithTimestampIndication(f1x::aasdk::messenger::Timestamp::ValueType, const f1x::aasdk::common::DataConstBuffer&) override {
        channel_->receive(this->shared_from_this());
    }
    void onAVMediaIndication(const f1x::aasdk::common::DataConstBuffer&) override {
        channel_->receive(this->shared_from_this());
    }
    void onChannelError(const f1x::aasdk::error::Error& e) override {
        std::cout << "[AudioInput] error: " << e.what() << std::endl;
    }

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::av::AudioServiceChannel::Pointer channel_;
};

// ---------- Stub Sensor Service ----------
class StubSensorService
    : public IService
    , public f1x::aasdk::channel::sensor::ISensorServiceChannelEventHandler
    , public std::enable_shared_from_this<StubSensorService>
{
public:
    StubSensorService(boost::asio::io_service& ios, f1x::aasdk::messenger::IMessenger::Pointer messenger)
        : strand_(ios)
        , channel_(std::make_shared<f1x::aasdk::channel::sensor::SensorServiceChannel>(strand_, std::move(messenger)))
    {}

    void start() override {
        std::cout << "[SensorService] start" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void stop() override {}

    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        auto* sc = ch->mutable_sensor_channel();
        auto* se = sc->add_sensors();
        se->set_type(f1x::aasdk::proto::enums::SensorType::DRIVING_STATUS);
    }

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest&) override {
        std::cout << "[SensorService] ChannelOpenRequest" << std::endl;
        f1x::aasdk::proto::messages::ChannelOpenResponse resp;
        resp.set_status(f1x::aasdk::proto::enums::Status::OK);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendChannelOpenResponse(resp, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onSensorStartRequest(const f1x::aasdk::proto::messages::SensorStartRequestMessage& req) override {
        std::cout << "[SensorService] SensorStartRequest type=" << req.sensor_type() << std::endl;
        // Send driving status event
        f1x::aasdk::proto::messages::SensorEventIndication event;
        auto* drivingStatus = event.add_driving_status();
        drivingStatus->set_status(f1x::aasdk::proto::enums::DrivingStatus::DRIVING_STATUS_UNRESTRICTED);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendSensorEventIndication(event, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onChannelError(const f1x::aasdk::error::Error& e) override {
        std::cout << "[SensorService] error: " << e.what() << std::endl;
    }

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::sensor::SensorServiceChannel::Pointer channel_;
};

// ---------- Stub Input Service ----------
class StubInputService
    : public IService
    , public f1x::aasdk::channel::input::IInputServiceChannelEventHandler
    , public std::enable_shared_from_this<StubInputService>
{
public:
    StubInputService(boost::asio::io_service& ios, f1x::aasdk::messenger::IMessenger::Pointer messenger)
        : strand_(ios)
        , channel_(std::make_shared<f1x::aasdk::channel::input::InputServiceChannel>(strand_, std::move(messenger)))
    {}

    void start() override {
        std::cout << "[InputService] start" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void stop() override {}

    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        auto* ic = ch->mutable_input_channel();
        auto* ts = ic->add_touch_screen_configs();
        ts->set_width(1920);
        ts->set_height(1080);
    }

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest&) override {
        std::cout << "[InputService] ChannelOpenRequest" << std::endl;
        f1x::aasdk::proto::messages::ChannelOpenResponse resp;
        resp.set_status(f1x::aasdk::proto::enums::Status::OK);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendChannelOpenResponse(resp, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onInputEvent(const f1x::aasdk::proto::messages::InputEventIndication& event) override {
        std::cout << "[InputService] InputEvent" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void onBindingRequest(const f1x::aasdk::proto::messages::BindingRequest& request) override {
        std::cout << "[InputService] BindingRequest" << std::endl;
        f1x::aasdk::proto::messages::BindingResponse resp;
        resp.set_status(f1x::aasdk::proto::enums::Status::OK);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendBindingResponse(resp, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onChannelError(const f1x::aasdk::error::Error& e) override {
        std::cout << "[InputService] error: " << e.what() << std::endl;
    }

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::input::InputServiceChannel::Pointer channel_;
};

// ---------- Stub Bluetooth Service ----------
class StubBluetoothService
    : public IService
    , public f1x::aasdk::channel::bluetooth::IBluetoothServiceChannelEventHandler
    , public std::enable_shared_from_this<StubBluetoothService>
{
public:
    StubBluetoothService(boost::asio::io_service& ios, f1x::aasdk::messenger::IMessenger::Pointer messenger)
        : strand_(ios)
        , channel_(std::make_shared<f1x::aasdk::channel::bluetooth::BluetoothServiceChannel>(strand_, std::move(messenger)))
    {}

    void start() override {
        std::cout << "[BluetoothService] start" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void stop() override {}

    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        ch->mutable_bluetooth_channel();
    }

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest&) override {
        std::cout << "[BluetoothService] ChannelOpenRequest" << std::endl;
        f1x::aasdk::proto::messages::ChannelOpenResponse resp;
        resp.set_status(f1x::aasdk::proto::enums::Status::OK);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendChannelOpenResponse(resp, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onBluetoothPairingRequest(const f1x::aasdk::proto::messages::BluetoothPairingRequest& req) override {
        std::cout << "[BluetoothService] PairingRequest" << std::endl;
        f1x::aasdk::proto::messages::BluetoothPairingResponse resp;
        resp.set_already_paired(true);
        resp.set_status(f1x::aasdk::proto::enums::BluetoothPairingStatus::OK);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendBluetoothPairingResponse(resp, std::move(promise));
        channel_->receive(this->shared_from_this());
    }
    void onChannelError(const f1x::aasdk::error::Error& e) override {
        std::cout << "[BluetoothService] error: " << e.what() << std::endl;
    }

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::bluetooth::BluetoothServiceChannel::Pointer channel_;
};

// ---------- Control Channel Event Handler (== AndroidAutoEntity logic) ----------
class HeadlessEntity
    : public f1x::aasdk::channel::control::IControlServiceChannelEventHandler
    , public std::enable_shared_from_this<HeadlessEntity>
{
public:
    typedef std::shared_ptr<IService> ServicePtr;
    typedef std::vector<ServicePtr> ServiceList;

    HeadlessEntity(boost::asio::io_service& ios,
                   f1x::aasdk::messenger::ICryptor::Pointer cryptor,
                   f1x::aasdk::transport::ITransport::Pointer transport,
                   f1x::aasdk::messenger::IMessenger::Pointer messenger,
                   ServiceList services)
        : strand_(ios)
        , cryptor_(std::move(cryptor))
        , transport_(std::move(transport))
        , messenger_(std::move(messenger))
        , controlChannel_(std::make_shared<f1x::aasdk::channel::control::ControlServiceChannel>(strand_, messenger_))
        , services_(std::move(services))
        , pingTimer_(ios)
    {}

    void start() {
        strand_.dispatch([this, self = shared_from_this()]() {
            std::cout << "[Entity] Starting services..." << std::endl;
            for (auto& s : services_) s->start();

            // Schedule ping
            schedulePing();

            // Send version request
            auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
            promise->then([](){
                std::cout << "[Entity] VersionRequest sent OK" << std::endl;
            }, [this](auto e){ onChannelError(e); });
            controlChannel_->sendVersionRequest(std::move(promise));
            controlChannel_->receive(shared_from_this());
        });
    }

    void stop() {
        for (auto& s : services_) s->stop();
        pingTimer_.cancel();
        messenger_->stop();
        transport_->stop();
        cryptor_->deinit();
    }

    // ---------- Control channel events ----------
    void onVersionResponse(uint16_t major, uint16_t minor,
                          f1x::aasdk::proto::enums::VersionResponseStatus::Enum status) override
    {
        std::cout << "[Entity] VersionResponse v" << major << "." << minor
                  << " status=" << status << std::endl;

        if (status == f1x::aasdk::proto::enums::VersionResponseStatus::MISMATCH) {
            std::cout << "[Entity] VERSION MISMATCH - quitting" << std::endl;
            return;
        }

        try {
            cryptor_->doHandshake();
            auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
            promise->then([](){
                std::cout << "[Entity] Handshake sent OK" << std::endl;
            }, [this](auto e){ onChannelError(e); });
            controlChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(promise));
            controlChannel_->receive(shared_from_this());
        } catch (const f1x::aasdk::error::Error& e) {
            std::cout << "[Entity] Handshake error: " << e.what() << std::endl;
        }
    }

    void onHandshake(const f1x::aasdk::common::DataConstBuffer& payload) override {
        std::cout << "[Entity] Handshake received, size=" << payload.size << std::endl;

        try {
            cryptor_->writeHandshakeBuffer(payload);

            if (!cryptor_->doHandshake()) {
                std::cout << "[Entity] Handshake continue..." << std::endl;
                auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
                promise->then([](){}, [this](auto e){ onChannelError(e); });
                controlChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(promise));
            } else {
                std::cout << "[Entity] AUTH COMPLETE!" << std::endl;
                f1x::aasdk::proto::messages::AuthCompleteIndication auth;
                auth.set_status(f1x::aasdk::proto::enums::Status::OK);
                auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
                promise->then([](){
                    std::cout << "[Entity] AuthComplete sent OK" << std::endl;
                }, [this](auto e){ onChannelError(e); });
                controlChannel_->sendAuthComplete(auth, std::move(promise));
            }
            controlChannel_->receive(shared_from_this());
        } catch (const f1x::aasdk::error::Error& e) {
            std::cout << "[Entity] Handshake processing error: " << e.what() << std::endl;
        }
    }

    void onServiceDiscoveryRequest(const f1x::aasdk::proto::messages::ServiceDiscoveryRequest& request) override {
        std::cout << "[Entity] ServiceDiscoveryRequest from: " << request.device_name()
                  << " / " << request.device_brand() << std::endl;

        f1x::aasdk::proto::messages::ServiceDiscoveryResponse response;
        response.mutable_channels()->Reserve(256);
        response.set_head_unit_name("OpenAuto");
        response.set_car_model("Universal");
        response.set_car_year("2018");
        response.set_car_serial("20180301");
        response.set_left_hand_drive_vehicle(true);
        response.set_headunit_manufacturer("f1x");
        response.set_headunit_model("OpenAuto Autoapp");
        response.set_sw_build("1");
        response.set_sw_version("1.0");
        response.set_can_play_native_media_during_vr(false);
        response.set_hide_clock(false);

        for (auto& s : services_) {
            s->fillFeatures(response);
        }

        std::cout << "[Entity] ServiceDiscoveryResponse: " << response.channels_size() << " channels, "
                  << response.ByteSizeLong() << " bytes" << std::endl;

        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){
            std::cout << "[Entity] ServiceDiscoveryResponse sent OK" << std::endl;
        }, [this](auto e){ onChannelError(e); });
        controlChannel_->sendServiceDiscoveryResponse(response, std::move(promise));
        controlChannel_->receive(shared_from_this());
    }

    void onAudioFocusRequest(const f1x::aasdk::proto::messages::AudioFocusRequest& request) override {
        std::cout << "[Entity] AudioFocusRequest type=" << request.audio_focus_type() << std::endl;

        auto state = (request.audio_focus_type() == f1x::aasdk::proto::enums::AudioFocusType::RELEASE)
            ? f1x::aasdk::proto::enums::AudioFocusState::LOSS
            : f1x::aasdk::proto::enums::AudioFocusState::GAIN;

        f1x::aasdk::proto::messages::AudioFocusResponse resp;
        resp.set_audio_focus_state(state);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        controlChannel_->sendAudioFocusResponse(resp, std::move(promise));
        controlChannel_->receive(shared_from_this());
    }

    void onShutdownRequest(const f1x::aasdk::proto::messages::ShutdownRequest& request) override {
        std::cout << "[Entity] ShutdownRequest reason=" << request.reason() << std::endl;
        f1x::aasdk::proto::messages::ShutdownResponse resp;
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){
            std::cout << "[Entity] ShutdownResponse sent, quitting." << std::endl;
        }, [this](auto e){ onChannelError(e); });
        controlChannel_->sendShutdownResponse(resp, std::move(promise));
    }

    void onShutdownResponse(const f1x::aasdk::proto::messages::ShutdownResponse&) override {
        std::cout << "[Entity] ShutdownResponse received, quitting." << std::endl;
    }

    void onNavigationFocusRequest(const f1x::aasdk::proto::messages::NavigationFocusRequest& request) override {
        std::cout << "[Entity] NavigationFocusRequest type=" << request.type() << std::endl;
        f1x::aasdk::proto::messages::NavigationFocusResponse resp;
        resp.set_type(2);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        controlChannel_->sendNavigationFocusResponse(resp, std::move(promise));
        controlChannel_->receive(shared_from_this());
    }

    void onPingResponse(const f1x::aasdk::proto::messages::PingResponse&) override {
        std::cout << "[Entity] PingResponse" << std::endl;
        controlChannel_->receive(shared_from_this());
    }

    void onChannelError(const f1x::aasdk::error::Error& e) override {
        std::cout << "[Entity] CHANNEL ERROR: " << e.what() << std::endl;
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
        f1x::aasdk::proto::messages::PingRequest req;
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        controlChannel_->sendPingRequest(req, std::move(promise));
    }

    boost::asio::io_service::strand strand_;
    f1x::aasdk::messenger::ICryptor::Pointer cryptor_;
    f1x::aasdk::transport::ITransport::Pointer transport_;
    f1x::aasdk::messenger::IMessenger::Pointer messenger_;
    f1x::aasdk::channel::control::ControlServiceChannel::Pointer controlChannel_;
    ServiceList services_;
    boost::asio::deadline_timer pingTimer_;
};

// ---------- Main ----------
int main(int argc, char* argv[])
{
    std::cout << "=== OpenAuto Headless TCP Server ===" << std::endl;
    std::cout << "Listening on port 5277 for wireless Android Auto..." << std::endl;

    boost::asio::io_service ioService;
    boost::asio::io_service::work work(ioService);

    // Worker threads
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&ioService]() { ioService.run(); });
    }

    // TCP acceptor
    tcp::acceptor acceptor(ioService, tcp::endpoint(tcp::v4(), 5277));
    acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    std::cout << "TCP server listening on :5277" << std::endl;

    while (true) {
        auto socket = std::make_shared<tcp::socket>(ioService);
        boost::system::error_code ec;
        acceptor.accept(*socket, ec);
        if (ec) {
            std::cout << "Accept error: " << ec.message() << std::endl;
            continue;
        }

        auto remote = socket->remote_endpoint();
        std::cout << "\n=== Connection from " << remote.address().to_string()
                  << ":" << remote.port() << " ===" << std::endl;

        // Create transport stack
        f1x::aasdk::tcp::TCPWrapper tcpWrapper;
        auto tcpEndpoint = std::make_shared<f1x::aasdk::tcp::TCPEndpoint>(tcpWrapper, std::move(socket));
        auto transport = std::make_shared<f1x::aasdk::transport::TCPTransport>(ioService, std::move(tcpEndpoint));

        // Create cryptor
        auto sslWrapper = std::make_shared<f1x::aasdk::transport::SSLWrapper>();
        auto cryptor = std::make_shared<f1x::aasdk::messenger::Cryptor>(std::move(sslWrapper));
        cryptor->init();

        // Create messenger
        auto messenger = std::make_shared<f1x::aasdk::messenger::Messenger>(
            ioService,
            std::make_shared<f1x::aasdk::messenger::MessageInStream>(ioService, transport, cryptor),
            std::make_shared<f1x::aasdk::messenger::MessageOutStream>(ioService, transport, cryptor));

        // Create stub services
        HeadlessEntity::ServiceList services;
        services.push_back(std::make_shared<StubAudioInputService>(ioService, messenger));
        services.push_back(std::make_shared<StubAudioService>(ioService, messenger,
            f1x::aasdk::messenger::ChannelId::MEDIA_AUDIO, "MediaAudio",
            f1x::aasdk::proto::enums::AudioType::MEDIA, 48000, 16, 2));
        services.push_back(std::make_shared<StubAudioService>(ioService, messenger,
            f1x::aasdk::messenger::ChannelId::SPEECH_AUDIO, "SpeechAudio",
            f1x::aasdk::proto::enums::AudioType::SPEECH, 16000, 16, 1));
        services.push_back(std::make_shared<StubAudioService>(ioService, messenger,
            f1x::aasdk::messenger::ChannelId::SYSTEM_AUDIO, "SystemAudio",
            f1x::aasdk::proto::enums::AudioType::SYSTEM, 16000, 16, 1));
        services.push_back(std::make_shared<StubSensorService>(ioService, messenger));
        services.push_back(std::make_shared<StubVideoService>(ioService, messenger));
        services.push_back(std::make_shared<StubBluetoothService>(ioService, messenger));
        services.push_back(std::make_shared<StubInputService>(ioService, messenger));

        // Create and start entity
        auto entity = std::make_shared<HeadlessEntity>(
            ioService, std::move(cryptor), std::move(transport),
            std::move(messenger), std::move(services));
        entity->start();

        std::cout << "Entity started, waiting for next connection..." << std::endl;
    }

    for (auto& t : threads) t.join();
    return 0;
}
