#!/usr/bin/env python3
"""Patch headless_tcp_server.cpp to use v1.6 ServiceDiscoveryResponse fields"""

src_path = '/tmp/headless-tcp-src/headless_tcp_server.cpp'
with open(src_path, 'r') as f:
    src = f.read()

fixes = 0

# Add new includes for the new proto messages
if 'HeadUnitInfoMessage.pb.h' not in src:
    src = src.replace(
        '#include <aasdk_proto/ServiceDiscoveryResponseMessage.pb.h>',
        '#include <aasdk_proto/ServiceDiscoveryResponseMessage.pb.h>\n'
        '#include <aasdk_proto/HeadUnitInfoMessage.pb.h>\n'
        '#include <aasdk_proto/ConnectionConfigurationMessage.pb.h>\n'
        '#include <aasdk_proto/PingConfigurationMessage.pb.h>\n'
        '#include <aasdk_proto/WirelessTcpConfigurationMessage.pb.h>'
    )
    fixes += 1

# Replace the old ServiceDiscoveryResponse field setting with v1.6 fields
old_response = '''        f1x::aasdk::proto::messages::ServiceDiscoveryResponse response;
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
        response.set_hide_clock(false);'''

new_response = '''        f1x::aasdk::proto::messages::ServiceDiscoveryResponse response;
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
        response.set_hide_clock(false);'''

if old_response in src:
    src = src.replace(old_response, new_response)
    fixes += 1

with open(src_path, 'w') as f:
    f.write(src)

print(f'Applied {fixes} fixes')
