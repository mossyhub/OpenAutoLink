#!/usr/bin/env python3
"""Fix remaining API issues in ocd_headless_test.cpp"""

path = '/tmp/ocd-test-src/ocd_headless_test.cpp'
with open(path, 'r') as f:
    src = f.read()

# VIDEO_RESOLUTION_1080P -> VIDEO_1920x1080
src = src.replace('VIDEO_RESOLUTION_1080P', 'VIDEO_1920x1080')

# VIDEO_FPS_60 is correct already

# set_sample_rate -> set_sampling_rate
src = src.replace('set_sample_rate', 'set_sampling_rate')

# AUDIO_STREAM_SYSTEM -> AUDIO_STREAM_SYSTEM_AUDIO
src = src.replace('AUDIO_STREAM_SYSTEM)', 'AUDIO_STREAM_SYSTEM_AUDIO)')

# sendByeByeResponse -> sendShutdownResponse
src = src.replace('sendByeByeResponse', 'sendShutdownResponse')

with open(path, 'w') as f:
    f.write(src)

print('Fixes applied')
