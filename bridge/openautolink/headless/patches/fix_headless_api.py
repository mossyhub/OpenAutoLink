#!/usr/bin/env python3
"""Fix all API mismatches in headless_tcp_server.cpp for aasdk on VIM4"""

src_path = '/tmp/headless-tcp-src/headless_tcp_server.cpp'
with open(src_path, 'r') as f:
    src = f.read()

fixes = 0

# Fix 1: Remove set_density (no such field in VideoConfig)
if 'set_density' in src:
    src = src.replace('        vc->set_density(160);\n', '')
    fixes += 1

# Fix 2: set_configs_count -> add_configs(0)
if 'set_configs_count' in src:
    src = src.replace('response.set_configs_count(1);', 'response.add_configs(0);')
    fixes += 1

# Fix 3: AVInputChannel add_audio_configs -> mutable_audio_config (singular)
if "av->add_audio_configs()" in src:
    src = src.replace("auto* cfg = av->add_audio_configs();", "auto* cfg = av->mutable_audio_config();")
    fixes += 1

# Fix 4: Replace AVInputOpenResponse with AVChannelSetupResponse for AudioInput
if 'sendAVInputOpenResponse' in src:
    old = """        f1x::aasdk::proto::messages::AVInputOpenResponse resp;
        resp.set_session(1);
        resp.set_value(0);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendAVInputOpenResponse(resp, std::move(promise));"""
    new = """        f1x::aasdk::proto::messages::AVChannelSetupResponse resp;
        resp.set_media_status(f1x::aasdk::proto::enums::AVChannelSetupStatus::OK);
        resp.set_max_unacked(1);
        resp.add_configs(0);
        auto promise = f1x::aasdk::channel::SendPromise::defer(strand_);
        promise->then([](){}, [this](auto e){ onChannelError(e); });
        channel_->sendAVChannelSetupResponse(resp, std::move(promise));"""
    src = src.replace(old, new)
    fixes += 1

# Fix 5: DrivingStatus field name
if 'DRIVING_STATUS_UNRESTRICTED' in src:
    src = src.replace(
        "drivingStatus->set_status(f1x::aasdk::proto::enums::DrivingStatus::DRIVING_STATUS_UNRESTRICTED);",
        "ds->set_status(static_cast<int32_t>(f1x::aasdk::proto::enums::DrivingStatus::UNRESTRICTED));"
    )
    fixes += 1

# Fix 6: variable name drivingStatus -> ds
if 'auto* drivingStatus = event.add_driving_status()' in src:
    src = src.replace('auto* drivingStatus = event.add_driving_status();', 'auto* ds = event.add_driving_status();')
    fixes += 1

# Fix 7: Remove onInputEvent (doesn't exist in IInputServiceChannelEventHandler)
if 'onInputEvent' in src:
    old_block = """    void onInputEvent(const f1x::aasdk::proto::messages::InputEventIndication& event) override {
        std::cout << "[InputService] InputEvent" << std::endl;
        channel_->receive(this->shared_from_this());
    }
    void onBindingRequest"""
    new_block = """    void onBindingRequest"""
    src = src.replace(old_block, new_block)
    fixes += 1

# Fix 8: add_touch_screen_configs -> mutable_touch_screen_config (singular)
if 'add_touch_screen_configs' in src:
    src = src.replace("auto* ts = ic->add_touch_screen_configs();", "auto* ts = ic->mutable_touch_screen_config();")
    fixes += 1

# Fix 9: Remove InputEventIndication include (not used)
if 'InputEventIndicationMessage.pb.h' in src:
    src = src.replace('#include <aasdk_proto/InputEventIndicationMessage.pb.h>\n', '')
    fixes += 1

# Fix 10: Add missing includes
if 'DrivingStatusData.pb.h' not in src:
    src = src.replace(
        '#include <aasdk_proto/SensorStartRequestMessage.pb.h>',
        '#include <aasdk_proto/SensorStartRequestMessage.pb.h>\n#include <aasdk_proto/DrivingStatusData.pb.h>\n#include <aasdk_proto/DrivingStatusEnum.pb.h>'
    )
    fixes += 1

if 'BindingRequestMessage.pb.h' not in src:
    src = src.replace(
        '#include <aasdk_proto/BluetoothPairingResponseMessage.pb.h>',
        '#include <aasdk_proto/BluetoothPairingResponseMessage.pb.h>\n#include <aasdk_proto/BindingRequestMessage.pb.h>\n#include <aasdk_proto/BindingResponseMessage.pb.h>'
    )
    fixes += 1

# Fix 11: SensorStartRequest -> SensorStartRequestMessage
if 'SensorStartRequest& req' in src and 'SensorStartRequestMessage& req' not in src:
    src = src.replace(
        'void onSensorStartRequest(const f1x::aasdk::proto::messages::SensorStartRequest& req)',
        'void onSensorStartRequest(const f1x::aasdk::proto::messages::SensorStartRequestMessage& req)'
    )
    fixes += 1

# Fix 12: Remove unnecessary forward declaration
fwd_decl = """namespace f1x { namespace aasdk { namespace channel { namespace control {
    class IControlServiceChannelEventHandler;
}}}}

"""
if fwd_decl in src:
    src = src.replace(fwd_decl, '')
    fixes += 1

with open(src_path, 'w') as f:
    f.write(src)

print(f'Applied {fixes} fixes')
