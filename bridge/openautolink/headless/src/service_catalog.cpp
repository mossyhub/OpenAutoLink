#include "openautolink/service_catalog.hpp"

#include <array>
#include <sstream>
#include <string_view>

#if defined(PI_AA_HAVE_LOCAL_AASDK_SERVICE_DISCOVERY_PROTO)
#include <AVChannelData.pb.h>
#include <AVInputChannelData.pb.h>
#include <AVStreamTypeEnum.pb.h>
#include <AudioConfigData.pb.h>
#include <AudioTypeEnum.pb.h>
#include <BluetoothChannelData.pb.h>
#include <BluetoothPairingMethodEnum.pb.h>
#include <ChannelDescriptorData.pb.h>
#include <InputChannelData.pb.h>
#include <SensorChannelData.pb.h>
#include <SensorData.pb.h>
#include <SensorTypeEnum.pb.h>
#include <ServiceDiscoveryResponseMessage.pb.h>
#include <TouchConfigData.pb.h>
#include <VideoConfigData.pb.h>
#include <VideoFPSEnum.pb.h>
#include <VideoResolutionEnum.pb.h>
#endif

#include "openautolink/contract.hpp"

namespace openautolink {

namespace {

enum class ServiceProtoChannelKind {
    AvInput,
    AvAudio,
    Sensor,
    Video,
    Bluetooth,
    Input,
};

struct ServiceDescriptor {
    int channel_id;
    std::string_view name;
    std::string_view channel;
    std::string_view source;
    std::string_view transport;
    int sample_rate_hz;
    int channels;
    bool requires_projection_replacement;
    ServiceProtoChannelKind proto_channel_kind;
};

struct ServiceDiscoveryProtoSnapshot {
    bool enabled = false;
    int serialized_size = 0;
    int channel_count = 0;
};

#if defined(PI_AA_HAVE_LOCAL_OPENAUTO_SOURCE)
constexpr bool kLocalOpenautoSourceEnabled = true;
#else
constexpr bool kLocalOpenautoSourceEnabled = false;
#endif

#if defined(PI_AA_HAVE_LOCAL_AASDK_SOURCE)
constexpr bool kLocalAasdkSourceEnabled = true;
#else
constexpr bool kLocalAasdkSourceEnabled = false;
#endif

#if defined(PI_AA_HAVE_LOCAL_AASDK_SERVICE_DISCOVERY_PROTO)
constexpr bool kLocalAasdkServiceDiscoveryProtoEnabled = true;
#else
constexpr bool kLocalAasdkServiceDiscoveryProtoEnabled = false;
#endif

constexpr std::array<ServiceDescriptor, 8> kOpenautoServiceCatalog = {{
    {1, "audio_input", "audio_input", "openauto::AudioInputService", "uplink", 16000, 1, true, ServiceProtoChannelKind::AvInput},
    {2, "media_audio", "av_media_audio", "openauto::MediaAudioService", "downlink", 48000, 2, true, ServiceProtoChannelKind::AvAudio},
    {3, "speech_audio", "av_speech_audio", "openauto::SpeechAudioService", "downlink", 16000, 1, true, ServiceProtoChannelKind::AvAudio},
    {4, "system_audio", "av_system_audio", "openauto::SystemAudioService", "downlink", 16000, 1, true, ServiceProtoChannelKind::AvAudio},
    {5, "sensor", "sensor", "openauto::SensorService", "bidirectional", 0, 0, false, ServiceProtoChannelKind::Sensor},
    {6, "video", "av_video", "openauto::VideoService", "downlink", 0, 0, true, ServiceProtoChannelKind::Video},
    {7, "bluetooth", "bluetooth", "openauto::BluetoothService", "bidirectional", 0, 0, true, ServiceProtoChannelKind::Bluetooth},
    {8, "input", "input", "openauto::InputService", "uplink", 0, 0, true, ServiceProtoChannelKind::Input},
}};

#if defined(PI_AA_HAVE_LOCAL_AASDK_SERVICE_DISCOVERY_PROTO)
void populate_audio_config(const ServiceDescriptor& service, aasdk::proto::data::AudioConfig* config)
{
    config->set_sample_rate(service.sample_rate_hz);
    config->set_bit_depth(16);
    config->set_channel_count(service.channels);
}

void populate_channel_descriptor(
    const ServiceDescriptor& service,
    aasdk::proto::data::ChannelDescriptor* descriptor
)
{
    descriptor->set_channel_id(service.channel_id);

    switch(service.proto_channel_kind) {
    case ServiceProtoChannelKind::AvInput: {
        auto* av_input_channel = descriptor->mutable_av_input_channel();
        av_input_channel->set_stream_type(aasdk::proto::enums::AVStreamType::AUDIO);
        populate_audio_config(service, av_input_channel->mutable_audio_config());
        av_input_channel->set_available_while_in_call(true);
        break;
    }
    case ServiceProtoChannelKind::AvAudio: {
        auto* av_channel = descriptor->mutable_av_channel();
        av_channel->set_stream_type(aasdk::proto::enums::AVStreamType::AUDIO);
        if(service.name == "media_audio") {
            av_channel->set_audio_type(aasdk::proto::enums::AudioType::MEDIA);
        } else if(service.name == "speech_audio") {
            av_channel->set_audio_type(aasdk::proto::enums::AudioType::SPEECH);
        } else {
            av_channel->set_audio_type(aasdk::proto::enums::AudioType::SYSTEM);
        }
        populate_audio_config(service, av_channel->add_audio_configs());
        av_channel->set_available_while_in_call(true);
        break;
    }
    case ServiceProtoChannelKind::Sensor: {
        auto* sensor_channel = descriptor->mutable_sensor_channel();
        sensor_channel->add_sensors()->set_type(aasdk::proto::enums::SensorType::DRIVING_STATUS);
        sensor_channel->add_sensors()->set_type(aasdk::proto::enums::SensorType::NIGHT_DATA);
        break;
    }
    case ServiceProtoChannelKind::Video: {
        auto* av_channel = descriptor->mutable_av_channel();
        av_channel->set_stream_type(aasdk::proto::enums::AVStreamType::VIDEO);
        auto* video_config = av_channel->add_video_configs();
        video_config->set_video_resolution(aasdk::proto::enums::VideoResolution::_720p);
        video_config->set_video_fps(aasdk::proto::enums::VideoFPS::_30);
        video_config->set_dpi(160);
        av_channel->set_available_while_in_call(true);
        break;
    }
    case ServiceProtoChannelKind::Bluetooth: {
        auto* bluetooth_channel = descriptor->mutable_bluetooth_channel();
        bluetooth_channel->set_adapter_address("02:00:00:00:00:01");
        bluetooth_channel->add_supported_pairing_methods(aasdk::proto::enums::BluetoothPairingMethod::A2DP);
        bluetooth_channel->add_supported_pairing_methods(aasdk::proto::enums::BluetoothPairingMethod::HFP);
        break;
    }
    case ServiceProtoChannelKind::Input: {
        auto* input_channel = descriptor->mutable_input_channel();
        input_channel->add_supported_keycodes(1);
        input_channel->add_supported_keycodes(2);
        input_channel->add_supported_keycodes(3);
        auto* touch_config = input_channel->mutable_touch_screen_config();
        touch_config->set_width(1280);
        touch_config->set_height(720);
        break;
    }
    }
}
#endif

ServiceDiscoveryProtoSnapshot build_service_discovery_proto_snapshot()
{
    ServiceDiscoveryProtoSnapshot snapshot;
    snapshot.enabled = kLocalAasdkServiceDiscoveryProtoEnabled;

#if defined(PI_AA_HAVE_LOCAL_AASDK_SERVICE_DISCOVERY_PROTO)
    aasdk::proto::messages::ServiceDiscoveryResponse response;
    response.set_head_unit_name("OpenAuto Headless");
    response.set_car_model("carlink_native");
    response.set_car_year("2026");
    response.set_car_serial("OpenAutoLink");
    response.set_left_hand_drive_vehicle(true);
    response.set_headunit_manufacturer("carlink_native");
    response.set_headunit_model("OpenAutoLink");
    response.set_sw_build("openauto-headless");
    response.set_sw_version("phase-oa-2");
    response.set_can_play_native_media_during_vr(true);
    response.set_hide_clock(false);

    for(const auto& service : kOpenautoServiceCatalog) {
        populate_channel_descriptor(service, response.add_channels());
    }

    snapshot.channel_count = response.channels_size();
    snapshot.serialized_size = static_cast<int>(response.ByteSizeLong());
#endif

    return snapshot;
}

} // namespace

std::string build_service_catalog_message(std::string_view phone_name)
{
    const auto proto_snapshot = build_service_discovery_proto_snapshot();

    std::ostringstream payload;
    payload << "{\"type\":\"media\",\"media_type\":902,\"payload\":{"
            << "\"CatalogType\":\"openauto_service_discovery\"," 
            << "\"PhoneName\":\"" << json_escape(phone_name) << "\"," 
            << "\"HeadUnitName\":\"OpenAuto Headless\"," 
            << "\"HeadUnitManufacturer\":\"carlink_native\"," 
            << "\"HeadUnitModel\":\"OpenAutoLink\"," 
            << "\"ServiceCount\":" << kOpenautoServiceCatalog.size() << ','
            << "\"LocalOpenautoSourceEnabled\":" << (kLocalOpenautoSourceEnabled ? "true" : "false") << ','
            << "\"LocalAasdkSourceEnabled\":" << (kLocalAasdkSourceEnabled ? "true" : "false") << ','
            << "\"LocalAasdkServiceDiscoveryProtoEnabled\":" << (proto_snapshot.enabled ? "true" : "false") << ','
            << "\"ServiceDiscoveryResponseProtoSize\":" << proto_snapshot.serialized_size << ','
            << "\"ServiceDiscoveryResponseProtoChannelCount\":" << proto_snapshot.channel_count << ','
            << "\"Services\":[";

    for(std::size_t index = 0; index < kOpenautoServiceCatalog.size(); ++index) {
        const auto& service = kOpenautoServiceCatalog[index];
        if(index > 0) {
            payload << ',';
        }

        payload << '{'
            << "\"ChannelId\":" << service.channel_id << ','
                << "\"Name\":\"" << json_escape(service.name) << "\"," 
                << "\"Channel\":\"" << json_escape(service.channel) << "\"," 
                << "\"Source\":\"" << json_escape(service.source) << "\"," 
                << "\"TransportDirection\":\"" << json_escape(service.transport) << "\"," 
                << "\"SampleRateHz\":" << service.sample_rate_hz << ','
                << "\"ChannelCount\":" << service.channels << ','
                << "\"RequiresProjectionReplacement\":" << (service.requires_projection_replacement ? "true" : "false")
                << '}';
    }

    payload << "]}}";
    return payload.str();
}

} // namespace openautolink
