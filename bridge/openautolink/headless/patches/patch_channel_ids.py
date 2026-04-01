#!/usr/bin/env python3
"""Patch headless_tcp_server.cpp to set channel_id on each ChannelDescriptor"""

src_path = '/tmp/headless-tcp-src/headless_tcp_server.cpp'
with open(src_path, 'r') as f:
    src = f.read()

fixes = 0

# Video Service: channel_id = 2 (VIDEO in old aasdk)
old = '''    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        auto* vi = ch->mutable_av_channel();
        vi->set_stream_type(f1x::aasdk::proto::enums::AVStreamType::VIDEO);'''
new = '''    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        ch->set_channel_id(static_cast<uint32_t>(f1x::aasdk::messenger::ChannelId::VIDEO));
        auto* vi = ch->mutable_av_channel();
        vi->set_stream_type(f1x::aasdk::proto::enums::AVStreamType::VIDEO);'''
if old in src:
    src = src.replace(old, new)
    fixes += 1

# Audio services: set channel_id based on channelId parameter
# For each StubAudioService, we need to set the channel_id in fillFeatures
# The channelId is stored as a member, but ChannelId enum -> uint32 cast
old_audio = '''    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        auto* ac = ch->mutable_av_channel();
        ac->set_stream_type(f1x::aasdk::proto::enums::AVStreamType::AUDIO);
        ac->set_audio_type(audioType_);'''
new_audio = '''    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& response) override {
        auto* ch = response.add_channels();
        ch->set_channel_id(static_cast<uint32_t>(channelId_));
        auto* ac = ch->mutable_av_channel();
        ac->set_stream_type(f1x::aasdk::proto::enums::AVStreamType::AUDIO);
        ac->set_audio_type(audioType_);'''
if old_audio in src:
    src = src.replace(old_audio, new_audio)
    fixes += 1

# StubAudioService needs channelId_ member - check if it exists
# It stores channel_ which is AudioServiceChannel, we need the raw ChannelId
# Let's add a channelId_ member
if 'channelId_(' not in src and 'f1x::aasdk::messenger::ChannelId channelId_' not in src:
    # Add channelId_ to StubAudioService constructor and member
    src = src.replace(
        '''    StubAudioService(boost::asio::io_service& ios, f1x::aasdk::messenger::IMessenger::Pointer messenger,
                     f1x::aasdk::messenger::ChannelId channelId, const std::string& name,
                     f1x::aasdk::proto::enums::AudioType::Enum audioType,
                     uint32_t sampleRate, uint32_t bitDepth, uint32_t channels)
        : strand_(ios)
        , channel_(std::make_shared<f1x::aasdk::channel::av::AudioServiceChannel>(strand_, std::move(messenger), channelId))
        , name_(name), audioType_(audioType), sampleRate_(sampleRate), bitDepth_(bitDepth), channelCount_(channels)''',
        '''    StubAudioService(boost::asio::io_service& ios, f1x::aasdk::messenger::IMessenger::Pointer messenger,
                     f1x::aasdk::messenger::ChannelId channelId, const std::string& name,
                     f1x::aasdk::proto::enums::AudioType::Enum audioType,
                     uint32_t sampleRate, uint32_t bitDepth, uint32_t channels)
        : strand_(ios)
        , channel_(std::make_shared<f1x::aasdk::channel::av::AudioServiceChannel>(strand_, std::move(messenger), channelId))
        , channelId_(channelId)
        , name_(name), audioType_(audioType), sampleRate_(sampleRate), bitDepth_(bitDepth), channelCount_(channels)'''
    )
    # Add the member variable
    src = src.replace(
        '''    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::av::AudioServiceChannel::Pointer channel_;
    std::string name_;''',
        '''    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::av::AudioServiceChannel::Pointer channel_;
    f1x::aasdk::messenger::ChannelId channelId_;
    std::string name_;'''
    )
    fixes += 1

# AudioInput: channel_id = AV_INPUT (7 in old enum)
old_ai = '''        auto* ch = response.add_channels();
        auto* av = ch->mutable_av_input_channel();'''
new_ai = '''        auto* ch = response.add_channels();
        ch->set_channel_id(static_cast<uint32_t>(f1x::aasdk::messenger::ChannelId::AV_INPUT));
        auto* av = ch->mutable_av_input_channel();'''
if old_ai in src:
    src = src.replace(old_ai, new_ai, 1)  # only first match (AudioInput)
    fixes += 1

# Sensor: channel_id = SENSOR (1)
old_sensor = '''        auto* ch = response.add_channels();
        auto* sc = ch->mutable_sensor_channel();'''
new_sensor = '''        auto* ch = response.add_channels();
        ch->set_channel_id(static_cast<uint32_t>(f1x::aasdk::messenger::ChannelId::SENSOR));
        auto* sc = ch->mutable_sensor_channel();'''
if old_sensor in src:
    src = src.replace(old_sensor, new_sensor, 1)
    fixes += 1

# Input: channel_id = INPUT (6 in old enum)
old_input = '''        auto* ch = response.add_channels();
        auto* ic = ch->mutable_input_channel();'''
new_input = '''        auto* ch = response.add_channels();
        ch->set_channel_id(static_cast<uint32_t>(f1x::aasdk::messenger::ChannelId::INPUT));
        auto* ic = ch->mutable_input_channel();'''
if old_input in src:
    src = src.replace(old_input, new_input, 1)
    fixes += 1

# Bluetooth: channel_id = BLUETOOTH (8)
# Find the bluetooth fillFeatures
old_bt = '''        auto* ch = response.add_channels();
        ch->mutable_bluetooth_channel();'''
new_bt = '''        auto* ch = response.add_channels();
        ch->set_channel_id(static_cast<uint32_t>(f1x::aasdk::messenger::ChannelId::BLUETOOTH));
        ch->mutable_bluetooth_channel();'''
if old_bt in src:
    src = src.replace(old_bt, new_bt, 1)
    fixes += 1

with open(src_path, 'w') as f:
    f.write(src)

print(f'Applied {fixes} fixes (set channel_id on all services)')
