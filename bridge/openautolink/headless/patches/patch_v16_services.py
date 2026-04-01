#!/usr/bin/env python3
"""Rewrite headless_tcp_server.cpp ServiceDiscoveryResponse to use v1.6 ServiceConfiguration"""

path = '/tmp/headless-tcp-src/headless_tcp_server.cpp'
with open(path, 'r') as f:
    src = f.read()

# Add new proto includes
new_includes = """#include <aasdk_proto/ServiceConfigurationV16.pb.h>
#include <aasdk_proto/VideoConfigurationV16.pb.h>
#include <aasdk_proto/MediaTypesV16.pb.h>"""

if 'ServiceConfigurationV16' not in src:
    src = src.replace(
        '#include <aasdk_proto/ServiceDiscoveryResponseMessage.pb.h>',
        '#include <aasdk_proto/ServiceDiscoveryResponseMessage.pb.h>\n' + new_includes
    )

# Replace the entire ServiceDiscovery response building section
# Find the block from "f1x::aasdk::proto::messages::ServiceDiscoveryResponse response;" to the logging line
old_block = '''        f1x::aasdk::proto::messages::ServiceDiscoveryResponse response;
        response.mutable_channels()->Reserve(256);

        // v1.6 fields
        response.set_driver_position(f1x::aasdk::proto::messages::DriverPosition::DRIVER_POSITION_LEFT);
        response.set_display_name("OpenAuto");
        response.set_probe_for_support(false);

        // Connection configuration (required for modern phones)
        auto* connConfig = response.mutable_connection_configuration();
        auto* pingConfig = connConfig->mutable_ping_configuration();
        pingConfig->set_timeout_ms(5000);
        pingConfig->set_interval_ms(1500);
        pingConfig->set_high_latency_threshold_ms(500);
        pingConfig->set_tracked_ping_count(5);

        // HeadUnit info (replaces deprecated top-level fields)
        auto* huInfo = response.mutable_headunit_info();
        huInfo->set_make("OpenAuto");
        huInfo->set_model("Universal");
        huInfo->set_year("2024");
        huInfo->set_vehicle_id("piaa-001");
        huInfo->set_head_unit_make("PiAA");
        huInfo->set_head_unit_model("Headless Bridge");
        huInfo->set_head_unit_software_build("1");
        huInfo->set_head_unit_software_version("1.0");

        // Keep deprecated fields for backward compat
        response.set_head_unit_name("OpenAuto");
        response.set_car_model("Universal");
        response.set_car_year("2024");
        response.set_car_serial("piaa-001");
        response.set_headunit_manufacturer("PiAA");
        response.set_headunit_model("Headless Bridge");
        response.set_sw_build("1");
        response.set_sw_version("1.0");
        response.set_can_play_native_media_during_vr(false);
        response.set_hide_clock(false);

        for (auto& s : services_) {
            s->fillFeatures(response);
        }'''

new_block = '''        f1x::aasdk::proto::messages::ServiceDiscoveryResponse response;
        response.mutable_channels()->Reserve(256);

        // v1.6 fields
        response.set_driver_position(f1x::aasdk::proto::messages::DriverPosition::DRIVER_POSITION_LEFT);
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

        // Keep deprecated fields
        response.set_head_unit_name("OpenAuto");
        response.set_car_model("Universal");
        response.set_car_year("2024");
        response.set_car_serial("piaa-001");
        response.set_headunit_manufacturer("PiAA");
        response.set_headunit_model("Headless Bridge");
        response.set_sw_build("1");
        response.set_sw_version("1.0");
        response.set_can_play_native_media_during_vr(false);
        response.set_hide_clock(false);

        // ---- V1.6 ServiceConfiguration channels ----
        using namespace f1x::aasdk::proto::data;

        // Video (MediaSinkService)
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(f1x::aasdk::messenger::ChannelId::VIDEO));
            auto* ms = svc->mutable_media_sink_service();
            ms->set_available_type(MediaCodecType::MEDIA_CODEC_VIDEO_H264_BP);
            ms->set_available_while_in_call(true);
            auto* vc = ms->add_video_configs();
            vc->set_codec_resolution(VideoCodecResolutionType::VIDEO_1920x1080);
            vc->set_frame_rate(VideoFrameRateType::VIDEO_FPS_60);
            vc->set_density(160);
            vc->set_height_margin(0);
            vc->set_width_margin(0);
        }

        // Media Audio
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(f1x::aasdk::messenger::ChannelId::MEDIA_AUDIO));
            auto* ms = svc->mutable_media_sink_service();
            ms->set_available_type(MediaCodecType::MEDIA_CODEC_AUDIO_PCM);
            ms->set_audio_type(AudioStreamType::AUDIO_STREAM_MEDIA);
            auto* ac = ms->add_audio_configs();
            ac->set_sampling_rate(48000);
            ac->set_number_of_bits(16);
            ac->set_number_of_channels(2);
        }

        // Speech/Guidance Audio
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(f1x::aasdk::messenger::ChannelId::SPEECH_AUDIO));
            auto* ms = svc->mutable_media_sink_service();
            ms->set_available_type(MediaCodecType::MEDIA_CODEC_AUDIO_PCM);
            ms->set_audio_type(AudioStreamType::AUDIO_STREAM_GUIDANCE);
            auto* ac = ms->add_audio_configs();
            ac->set_sampling_rate(16000);
            ac->set_number_of_bits(16);
            ac->set_number_of_channels(1);
        }

        // System Audio
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(f1x::aasdk::messenger::ChannelId::SYSTEM_AUDIO));
            auto* ms = svc->mutable_media_sink_service();
            ms->set_available_type(MediaCodecType::MEDIA_CODEC_AUDIO_PCM);
            ms->set_audio_type(AudioStreamType::AUDIO_STREAM_SYSTEM_AUDIO);
            auto* ac = ms->add_audio_configs();
            ac->set_sampling_rate(16000);
            ac->set_number_of_bits(16);
            ac->set_number_of_channels(1);
        }

        // Audio Input (MediaSourceService)
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(f1x::aasdk::messenger::ChannelId::AV_INPUT));
            auto* msrc = svc->mutable_media_source_service();
            msrc->set_available_type(MediaCodecType::MEDIA_CODEC_AUDIO_PCM);
            auto* ac = msrc->mutable_audio_config();
            ac->set_sampling_rate(16000);
            ac->set_number_of_bits(16);
            ac->set_number_of_channels(1);
        }

        // Sensor
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(f1x::aasdk::messenger::ChannelId::SENSOR));
            auto* ss = svc->mutable_sensor_source_service();
            auto* sensor = ss->add_sensors();
            sensor->set_sensor_type(SensorType16::SENSOR_DRIVING_STATUS_DATA);
        }

        // Input
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(f1x::aasdk::messenger::ChannelId::INPUT));
            auto* is = svc->mutable_input_source_service();
            auto* ts = is->add_touchscreen();
            ts->set_width(1920);
            ts->set_height(1080);
        }

        // Bluetooth
        {
            auto* svc = response.add_channels();
            svc->set_id(static_cast<int32_t>(f1x::aasdk::messenger::ChannelId::BLUETOOTH));
            auto* bs = svc->mutable_bluetooth_service();
            bs->set_car_address("CC:8D:A2:0C:5C:EA");
        }'''

if old_block in src:
    src = src.replace(old_block, new_block)
    print('Replaced ServiceDiscovery block')
else:
    print('WARNING: Could not find SDR block to replace')

# Remove old service fillFeatures calls and service class stubs
# (they are now inline in the SDR builder above)

with open(path, 'w') as f:
    f.write(src)

print('Done')
