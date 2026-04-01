#!/usr/bin/env python3
"""Make all stub service fillFeatures methods no-ops since SDR is built inline"""

path = '/tmp/headless-tcp-src/headless_tcp_server.cpp'
with open(path, 'r') as f:
    src = f.read()

# Replace each service's fillFeatures with a no-op
# We need to find each fillFeatures body and empty it

import re

# Pattern: void fillFeatures(...) override { ... }
# We need to handle multi-line bodies
# Simpler: find each fillFeatures and replace the body

# The stub services are: StubVideoService, StubAudioService, StubAudioInputService,
# StubSensorService, StubInputService, StubBluetoothService

# Each has a fillFeatures like:
#     void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
#         ... multiple lines ...
#     }

# Replace each block. Find all fillFeatures implementations
lines = src.split('\n')
result = []
in_fill_features = False
brace_depth = 0
skip_count = 0

for i, line in enumerate(lines):
    if 'void fillFeatures' in line and 'override' in line:
        # Start of a fillFeatures method
        result.append('    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {')
        result.append('        // Services are configured inline in the SDR builder')
        result.append('    }')
        in_fill_features = True
        brace_depth = 0
        # Count braces in this line
        brace_depth += line.count('{') - line.count('}')
        continue
    
    if in_fill_features:
        brace_depth += line.count('{') - line.count('}')
        if brace_depth <= 0:
            in_fill_features = False
        continue
    
    result.append(line)

src = '\n'.join(result)

with open(path, 'w') as f:
    f.write(src)

print('fillFeatures methods emptied')
