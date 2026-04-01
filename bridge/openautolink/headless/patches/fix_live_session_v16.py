#!/usr/bin/env python3
"""Fix remaining compilation issues in live_session.cpp for v1.6 proto"""

path = '/opt/pi-aa/openauto-headless/src/live_session.cpp'
with open(path, 'r') as f:
    src = f.read()

fixes = 0

# Fix 1: bt_mac -> hardcoded value (no bt_mac field in HeadlessConfig)
src = src.replace('config_.bt_mac', '"CC:8D:A2:0C:5C:EA"')
fixes += 1

# Fix 2: left_hand_drive_vehicle referenced in debug log
src = src.replace(
    '''              << " lhd=" << response.left_hand_drive_vehicle() << std::endl;''',
    '''              << " driver_pos=" << response.driver_position() << std::endl;'''
)
fixes += 1

# Fix 3: Empty all fillFeatures methods since SDR is built inline now
# We need to find each fillFeatures body and replace it with {}

import re

# Pattern to match fillFeatures method bodies
# Each one starts with "void Headless...Handler::fillFeatures(" and ends at the matched closing brace
handlers = [
    'HeadlessVideoHandler',
    'HeadlessAudioHandler', 
    'HeadlessAudioInputHandler',
    'HeadlessSensorHandler',
    'HeadlessInputHandler',
    'HeadlessBluetoothHandler',
]

for handler in handlers:
    # Find the start of fillFeatures for this handler
    pattern = f'void {handler}::fillFeatures(\n'
    idx = src.find(pattern)
    if idx < 0:
        pattern = f'void {handler}::fillFeatures('
        idx = src.find(pattern)
    if idx < 0:
        print(f'WARNING: Could not find fillFeatures for {handler}')
        continue
    
    # Find the opening brace
    brace_start = src.find('{', idx)
    if brace_start < 0:
        continue
    
    # Find the matching closing brace
    depth = 1
    pos = brace_start + 1
    while pos < len(src) and depth > 0:
        if src[pos] == '{':
            depth += 1
        elif src[pos] == '}':
            depth -= 1
        pos += 1
    
    # Replace the body with just {}
    # Keep the function signature
    sig_end = brace_start  # position of {
    old_body = src[sig_end:pos]
    new_body = '{\n    // v1.6: SDR built inline in onServiceDiscoveryRequest\n}'
    src = src[:sig_end] + new_body + src[pos:]
    fixes += 1
    print(f'Emptied fillFeatures for {handler}')

with open(path, 'w') as f:
    f.write(src)

print(f'Applied {fixes} fixes')
