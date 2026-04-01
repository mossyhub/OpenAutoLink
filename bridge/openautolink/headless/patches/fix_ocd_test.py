#!/usr/bin/env python3
"""Fix API issues in ocd_headless_test.cpp"""

path = '/tmp/ocd-test-src/ocd_headless_test.cpp'
with open(path, 'r') as f:
    src = f.read()

# Fix 1: PingRequest uses timestamp (int64), not data
src = src.replace(
    'req.set_data(42);',
    'req.set_timestamp(42);'
)

# Fix 2: PingResponse uses timestamp not data echo
src = src.replace(
    'resp.set_data(request.data());',
    'resp.set_timestamp(request.timestamp());'
)

# Fix 3: AudioFocusNotification uses set_focus_state not set_audio_focus_state
src = src.replace(
    'resp.set_audio_focus_state(\n            aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN);',
    'resp.set_focus_state(\n            aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN);'
)

# Fix 4: NavFocusNotification uses set_focus_type not set_type  
src = src.replace(
    'resp.set_focus_type(aap_protobuf::service::control::message::NAV_FOCUS_PROJECTED);',
    'resp.set_focus_type(aap_protobuf::service::control::message::NAV_FOCUS_PROJECTED);'
)
# That one is actually correct already

# Fix 5: Sensor uses set_sensor_type not set_type, and correct enum
src = src.replace(
    'sensor->set_type(\n                aap_protobuf::service::sensorsource::message::SENSOR_TYPE_DRIVING_STATUS);',
    'sensor->set_sensor_type(\n                aap_protobuf::service::sensorsource::message::SENSOR_DRIVING_STATUS_DATA);'
)

# Fix 6: AuthResponse uses set_status with int32, STATUS_SUCCESS = 0
src = src.replace(
    'auth.set_status(aap_protobuf::shared::STATUS_SUCCESS);',
    'auth.set_status(static_cast<int32_t>(aap_protobuf::shared::STATUS_SUCCESS));'
)

# Fix 7: sendByeByeResponse - check if method exists
# The method should be correct

# Fix 8: ByeByeRequest include 
if 'ByeByeRequest.pb.h' not in src:
    src = src.replace(
        '#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>',
        '#include <aap_protobuf/service/control/message/ByeByeRequest.pb.h>\n#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>'
    )

# Fix 9: Include SensorType
src = src.replace(
    '#include <aap_protobuf/service/sensorsource/message/Sensor.pb.h>',
    '#include <aap_protobuf/service/sensorsource/message/Sensor.pb.h>\n#include <aap_protobuf/service/sensorsource/message/SensorType.pb.h>'
)

with open(path, 'w') as f:
    f.write(src)

print('Fixes applied')
