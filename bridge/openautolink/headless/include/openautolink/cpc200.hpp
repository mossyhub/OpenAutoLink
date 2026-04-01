#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace openautolink {

// CPC200-CCPA USB Protocol — 16-byte header + variable payload
static constexpr uint32_t CPC200_MAGIC = 0x55AA55AA;
static constexpr size_t   CPC200_HEADER_SIZE = 16;
static constexpr size_t   CPC200_VIDEO_HEADER_SIZE = 20;  // width/height/encoder_state/pts/flags

// Message types (host ↔ adapter)
enum class CpcMessageType : uint32_t {
    OPEN              = 0x01,
    PLUGGED           = 0x02,
    PHASE             = 0x03,
    UNPLUGGED         = 0x04,
    TOUCH             = 0x05,  // AA single-touch (action/x/y/flags, 16 bytes)
    VIDEO_DATA        = 0x06,
    AUDIO_DATA        = 0x07,
    COMMAND           = 0x08,
    FRAME             = 0x0C,  // keyframe request (OUT) / BT PIN (IN)
    DISCONNECT_PHONE  = 0x0F,
    CLOSE_DONGLE      = 0x15,
    MULTI_TOUCH       = 0x17,
    BOX_SETTINGS      = 0x19,
    GNSS_DATA         = 0x29,
    MEDIA_DATA        = 0x2A,
    NAVI_VIDEO_DATA   = 0x2C,
    VEHICLE_DATA      = 0x30,
    NAVI_FOCUS_REQ    = 0x6E,
    NAVI_FOCUS_REL    = 0x6F,
    SEND_FILE         = 0x99,
    SESSION_TOKEN     = 0xA3,
    HEARTBEAT         = 0xAA,
    STATUS_VALUE      = 0xBB,
    SOFTWARE_VERSION  = 0xCC,
    HEARTBEAT_ECHO    = 0xCD,
};

// Command IDs sent in COMMAND payload
enum class CpcCommand : uint32_t {
    START_RECORD_AUDIO = 1,
    STOP_RECORD_AUDIO  = 2,
    SIRI               = 5,
    SIRI_BUTTON_UP     = 6,
    FRAME              = 12,
    START_GNSS_REPORT  = 18,
    STOP_GNSS_REPORT   = 19,
    AUDIO_TRANSFER_ON  = 22,
    AUDIO_TRANSFER_OFF = 23,
    WIFI_ENABLE        = 1000,
    WIFI_CONNECT       = 1002,
    SCANNING_DEVICE    = 1003,
    DEVICE_FOUND       = 1004,
    BT_CONNECTED       = 1007,
    WIFI_CONNECTED     = 1009,
    WIFI_DISCONNECTED  = 1010,
    REQUEST_VIDEO_FOCUS      = 500,
    RELEASE_VIDEO_FOCUS      = 501,
    REQUEST_NAVI_SCREEN_FOCUS = 508,
    RELEASE_NAVI_SCREEN_FOCUS = 509,
};

// ── Header pack / parse ────────────────────────────────────────────────

struct CpcHeader {
    uint32_t payload_length;
    uint32_t message_type;
    uint32_t type_check;
};

inline CpcHeader make_header(CpcMessageType type, uint32_t payload_length) {
    uint32_t t = static_cast<uint32_t>(type);
    return { payload_length, t, ~t };
}

// Pack header into 16 bytes (little-endian).
inline void pack_header(uint8_t* dst, const CpcHeader& h) {
    uint32_t magic = CPC200_MAGIC;
    memcpy(dst + 0,  &magic,           4);
    memcpy(dst + 4,  &h.payload_length, 4);
    memcpy(dst + 8,  &h.message_type,   4);
    memcpy(dst + 12, &h.type_check,     4);
}

// Parse header from 16 bytes.  Returns false on bad magic/checksum.
inline bool parse_header(const uint8_t* src, CpcHeader& out) {
    uint32_t magic;
    memcpy(&magic, src, 4);
    if (magic != CPC200_MAGIC) return false;
    memcpy(&out.payload_length,  src + 4, 4);
    memcpy(&out.message_type,    src + 8, 4);
    memcpy(&out.type_check,      src + 12, 4);
    uint32_t expected = ~out.message_type;
    return out.type_check == expected;
}

// ── Packet: header + payload in a single buffer ────────────────────────

struct CpcPacket {
    CpcMessageType type;
    std::vector<uint8_t> payload;

    // Serialize to wire format (header + payload).
    std::vector<uint8_t> pack() const {
        CpcHeader h = make_header(type, static_cast<uint32_t>(payload.size()));
        std::vector<uint8_t> buf(CPC200_HEADER_SIZE + payload.size());
        pack_header(buf.data(), h);
        if (!payload.empty())
            memcpy(buf.data() + CPC200_HEADER_SIZE, payload.data(), payload.size());
        return buf;
    }
};

// ── Payload helpers ────────────────────────────────────────────────────

// Pack a 4-byte LE integer payload.
inline std::vector<uint8_t> pack_int32_payload(uint32_t value) {
    std::vector<uint8_t> v(4);
    memcpy(v.data(), &value, 4);
    return v;
}

// Pack PLUGGED payload: [phone_type:4][wifi:4]
inline std::vector<uint8_t> pack_plugged_payload(uint32_t phone_type, uint32_t wifi_enabled) {
    std::vector<uint8_t> v(8);
    memcpy(v.data(), &phone_type, 4);
    memcpy(v.data() + 4, &wifi_enabled, 4);
    return v;
}

// Pack UTF-8 string payload (null terminated).
inline std::vector<uint8_t> pack_utf8_payload(const std::string& s) {
    std::vector<uint8_t> v(s.begin(), s.end());
    v.push_back(0);
    return v;
}

// Pack JSON string payload (no null terminator).
inline std::vector<uint8_t> pack_json_payload(const std::string& json) {
    return std::vector<uint8_t>(json.begin(), json.end());
}

// Build CPC200 VIDEO_DATA payload (20-byte header + H.264 data).
// Returns only the payload — caller wraps with pack_header(VIDEO_DATA).
inline void build_video_payload(
    uint8_t* dst,
    uint32_t width, uint32_t height,
    uint32_t encoder_state, uint32_t pts_ms, uint32_t flags,
    const uint8_t* h264_data, size_t h264_size)
{
    memcpy(dst + 0,  &width, 4);
    memcpy(dst + 4,  &height, 4);
    memcpy(dst + 8,  &encoder_state, 4);
    memcpy(dst + 12, &pts_ms, 4);
    memcpy(dst + 16, &flags, 4);
    memcpy(dst + CPC200_VIDEO_HEADER_SIZE, h264_data, h264_size);
}

// Parse COMMAND payload → command_id.
inline bool parse_command_payload(const uint8_t* data, size_t len, uint32_t& cmd_id) {
    if (len < 4) return false;
    memcpy(&cmd_id, data, 4);
    return true;
}

// Parse OPEN payload → width, height, fps, dpi, format, phone_type, padding.
inline bool parse_open_payload(const uint8_t* data, size_t len,
    uint32_t& width, uint32_t& height, uint32_t& fps,
    uint32_t& dpi, uint32_t& format, uint32_t& phone_type)
{
    if (len < 24) return false;
    memcpy(&width,      data + 0, 4);
    memcpy(&height,     data + 4, 4);
    memcpy(&fps,        data + 8, 4);
    memcpy(&format,     data + 12, 4);
    memcpy(&phone_type, data + 16, 4);
    memcpy(&dpi,        data + 20, 4);
    return true;
}

} // namespace openautolink
