#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace openautolink {

enum class InputType {
    Unknown,
    HostCommand,
    HostPacket,
    Touch,
    AudioInput,
    Gnss,
    VehicleData,
};

struct ParsedInputMessage {
    InputType type = InputType::Unknown;
    std::optional<int> command_id;
    std::optional<std::string> message_name;
    std::optional<std::string> payload_b64;
};

std::string json_escape(std::string_view value);
std::string base64_encode(const std::string& input);
std::string base64_decode(std::string_view input);
std::string hex_to_bytes(std::string_view hex);

std::optional<std::string> extract_string_field(std::string_view line, std::string_view key);
std::optional<int> extract_int_field(std::string_view line, std::string_view key);
ParsedInputMessage parse_input_message(std::string_view line);

std::string build_event_message(std::string_view event_type, std::string_view phone_name);
std::string build_media_message(
    int heartbeat_tick,
    bool playback_paused,
    int touch_count,
    int microphone_uplink_count,
    int gnss_count,
    std::string_view last_gnss_sentence
);
std::string build_json_media_message(int media_type, std::string_view payload_json);
std::string build_audio_message(int audio_type = 1, int decode_type = 4, double volume = 0.0);
std::string build_video_message(
    int pts_ms,
    bool force_idr,
    std::string_view message_name = "VIDEO_DATA",
    int width = 1920,
    int height = 1080
);
std::string build_audio_command_message(int command_id, int audio_type = 1, int decode_type = 4, double volume = 0.0);
std::string build_ducking_message(double target_volume, double duration_s, int audio_type = 1, int decode_type = 4);
std::string build_command_message(int command_id);
std::string build_navi_focus_message(bool is_request);

} // namespace openautolink
