#include "openautolink/contract.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>

namespace openautolink {

namespace {

constexpr std::string_view kMainIdrHex =
    "000000016742c01eda02802dd0808080a1"
    "0000000168ce06e2"
    "000000016588843a245000127e40";

constexpr std::string_view kMainPframeHex = "00000001419a22110005fe";

bool line_has_field_value(std::string_view line, std::string_view key, std::string_view value)
{
    const auto extracted = extract_string_field(line, key);
    return extracted.has_value() && *extracted == value;
}

} // namespace

std::string json_escape(std::string_view value)
{
    std::ostringstream escaped;
    for(char ch : value) {
        switch(ch) {
        case '\\':
            escaped << "\\\\";
            break;
        case '"':
            escaped << "\\\"";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            escaped << ch;
            break;
        }
    }
    return escaped.str();
}

std::string base64_encode(const std::string& input)
{
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    std::size_t index = 0;
    while(index < input.size()) {
        const std::uint32_t octet_a = index < input.size() ? static_cast<unsigned char>(input[index++]) : 0;
        const std::uint32_t octet_b = index < input.size() ? static_cast<unsigned char>(input[index++]) : 0;
        const std::uint32_t octet_c = index < input.size() ? static_cast<unsigned char>(input[index++]) : 0;

        const std::uint32_t triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;

        output.push_back(table[(triple >> 18U) & 0x3FU]);
        output.push_back(table[(triple >> 12U) & 0x3FU]);
        output.push_back(index > input.size() + 1 ? '=' : table[(triple >> 6U) & 0x3FU]);
        output.push_back(index > input.size() ? '=' : table[triple & 0x3FU]);
    }

    const auto remainder = input.size() % 3;
    if(remainder > 0) {
        output[output.size() - 1] = '=';
        if(remainder == 1) {
            output[output.size() - 2] = '=';
        }
    }

    return output;
}

std::string base64_decode(std::string_view input)
{
    auto decode_char = [](char ch) -> int {
        if(ch >= 'A' && ch <= 'Z') {
            return ch - 'A';
        }
        if(ch >= 'a' && ch <= 'z') {
            return ch - 'a' + 26;
        }
        if(ch >= '0' && ch <= '9') {
            return ch - '0' + 52;
        }
        if(ch == '+') {
            return 62;
        }
        if(ch == '/') {
            return 63;
        }
        return -1;
    };

    std::string output;
    output.reserve((input.size() * 3) / 4);

    int value = 0;
    int bits = -8;
    for(char ch : input) {
        if(ch == '=') {
            break;
        }

        const int decoded = decode_char(ch);
        if(decoded < 0) {
            continue;
        }

        value = (value << 6) | decoded;
        bits += 6;

        if(bits >= 0) {
            output.push_back(static_cast<char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }

    return output;
}

std::string hex_to_bytes(std::string_view hex)
{
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for(std::size_t index = 0; index + 1 < hex.size(); index += 2) {
        const auto byte = static_cast<char>(std::strtoul(std::string(hex.substr(index, 2)).c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

std::optional<std::string> extract_string_field(std::string_view line, std::string_view key)
{
    const auto pattern = "\"" + std::string(key) + "\":\"";
    const auto start = line.find(pattern);
    if(start == std::string_view::npos) {
        return std::nullopt;
    }

    const auto value_start = start + pattern.size();
    auto value_end = value_start;
    while(value_end < line.size()) {
        if(line[value_end] == '"' && line[value_end - 1] != '\\') {
            break;
        }
        ++value_end;
    }
    if(value_end >= line.size()) {
        return std::nullopt;
    }
    return std::string(line.substr(value_start, value_end - value_start));
}

std::optional<int> extract_int_field(std::string_view line, std::string_view key)
{
    const auto pattern = "\"" + std::string(key) + "\":";
    const auto start = line.find(pattern);
    if(start == std::string_view::npos) {
        return std::nullopt;
    }

    auto value_start = start + pattern.size();
    auto value_end = value_start;
    while(value_end < line.size() && (std::isdigit(static_cast<unsigned char>(line[value_end])) || line[value_end] == '-')) {
        ++value_end;
    }
    if(value_end == value_start) {
        return std::nullopt;
    }
    return std::stoi(std::string(line.substr(value_start, value_end - value_start)));
}

ParsedInputMessage parse_input_message(std::string_view line)
{
    ParsedInputMessage message;

    if(line_has_field_value(line, "type", "host_command")) {
        message.type = InputType::HostCommand;
        message.command_id = extract_int_field(line, "command_id");
        return message;
    }

    if(line_has_field_value(line, "type", "host_packet")) {
        message.type = InputType::HostPacket;
        message.message_name = extract_string_field(line, "message_name");
        message.payload_b64 = extract_string_field(line, "payload_b64");
        return message;
    }

    if(line_has_field_value(line, "type", "touch")) {
        message.type = InputType::Touch;
        message.payload_b64 = extract_string_field(line, "payload_b64");
        return message;
    }

    if(line_has_field_value(line, "type", "audio_input")) {
        message.type = InputType::AudioInput;
        message.payload_b64 = extract_string_field(line, "payload_b64");
        return message;
    }

    if(line_has_field_value(line, "type", "gnss")) {
        message.type = InputType::Gnss;
        message.payload_b64 = extract_string_field(line, "payload_b64");
        return message;
    }

    if(line_has_field_value(line, "type", "vehicle_data")) {
        message.type = InputType::VehicleData;
        message.payload_b64 = extract_string_field(line, "payload_b64");
        return message;
    }

    return message;
}

std::string build_event_message(std::string_view event_type, std::string_view phone_name)
{
    return "{\"type\":\"event\",\"event_type\":\"" + json_escape(event_type)
        + "\",\"phone_name\":\"" + json_escape(phone_name) + "\"}";
}

std::string build_media_message(
    int heartbeat_tick,
    bool playback_paused,
    int touch_count,
    int microphone_uplink_count,
    int gnss_count,
    std::string_view last_gnss_sentence
)
{
    std::ostringstream payload;
    payload
        << "{\"type\":\"media\",\"media_type\":1,\"payload\":{"
        << "\"MediaSongName\":\"Headless Stub\","
        << "\"MediaArtistName\":\"OpenAutoLink\","
        << "\"MediaAlbumName\":\"Backend Scaffold\","
        << "\"MediaAPPName\":\"OpenAutoLink\","
        << "\"MediaSongPlayTime\":" << (heartbeat_tick * 1000) << ','
        << "\"MediaPlayStatus\":" << (playback_paused ? 0 : 1) << ','
        << "\"TouchCount\":" << touch_count << ','
        << "\"MicrophoneUplinkCount\":" << microphone_uplink_count << ','
        << "\"GnssCount\":" << gnss_count << ','
        << "\"LastGnssSentence\":\"" << json_escape(last_gnss_sentence) << "\""
        << "}}";
    return payload.str();
}

std::string build_json_media_message(int media_type, std::string_view payload_json)
{
    std::ostringstream payload;
    payload << "{\"type\":\"media\",\"media_type\":" << media_type
            << ",\"payload\":" << payload_json << '}';
    return payload.str();
}

std::string build_audio_message(int audio_type, int decode_type, double volume)
{
    static const std::string pcm(480 * 2 * 2, '\x00');
    std::ostringstream payload;
    payload << "{\"type\":\"audio\",\"decode_type\":" << decode_type
            << ",\"audio_type\":" << audio_type
            << ",\"volume\":" << volume
            << ",\"data_b64\":\"" << base64_encode(pcm) << "\"}";
    return payload.str();
}

std::string build_video_message(int pts_ms, bool force_idr, std::string_view message_name, int width, int height)
{
    const auto bytes = force_idr ? hex_to_bytes(kMainIdrHex) : hex_to_bytes(kMainPframeHex);
    const auto flags = force_idr ? 1 : 0;
    std::ostringstream payload;
    payload
        << "{\"type\":\"video\",\"message_name\":\"" << json_escape(message_name) << "\"," 
        << "\"width\":" << width << ",\"height\":" << height << ",\"pts_ms\":" << pts_ms << ','
        << "\"encoder_state\":3,\"flags\":" << flags << ','
        << "\"data_b64\":\"" << base64_encode(bytes) << "\"}";
    return payload.str();
}

std::string build_audio_command_message(int command_id, int audio_type, int decode_type, double volume)
{
    std::ostringstream payload;
    payload << "{\"type\":\"audio_command\",\"command\":" << command_id
            << ",\"decode_type\":" << decode_type
            << ",\"audio_type\":" << audio_type
            << ",\"volume\":" << volume << '}';
    return payload.str();
}

std::string build_ducking_message(double target_volume, double duration_s, int audio_type, int decode_type)
{
    std::ostringstream payload;
    payload << "{\"type\":\"ducking\",\"target_volume\":" << target_volume
            << ",\"duration_s\":" << duration_s
            << ",\"decode_type\":" << decode_type
            << ",\"audio_type\":" << audio_type << '}';
    return payload.str();
}

std::string build_command_message(int command_id)
{
    std::ostringstream payload;
    payload << "{\"type\":\"command\",\"command_id\":" << command_id << '}';
    return payload.str();
}

std::string build_navi_focus_message(bool is_request)
{
    return std::string("{\"type\":\"navi_focus\",\"is_request\":")
        + (is_request ? "true}" : "false}");
}

} // namespace openautolink
