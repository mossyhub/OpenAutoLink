#include "openautolink/carplay_session.hpp"
#include "openautolink/oal_session.hpp"
#include "openautolink/oal_protocol.hpp"

#include <cstring>
#include <iostream>
#include <sstream>

namespace openautolink {

CarPlaySession::CarPlaySession(OalSession& oal, HeadlessConfig config)
    : oal_(oal)
    , config_(std::move(config))
    , bonjour_({
        .device_name = config_.head_unit_name,
        .device_id = {},
        .rtsp_port = 5000,
        .airplay_port = 7000,
        .model = "OpenAutoLink,1"
      })
    , rtsp_(5000)
    , crypto_()
    , airplay_({
        .video_port = 7000,
        .audio_port = 7001,
        .video_width = static_cast<uint16_t>(config_.video_width),
        .video_height = static_cast<uint16_t>(config_.video_height)
      }, crypto_)
{
}

CarPlaySession::~CarPlaySession()
{
    stop();
}

void CarPlaySession::start()
{
    if (running_.load()) return;
    running_.store(true);

    // Initialize crypto identity (load or generate Ed25519 keys)
    if (!crypto_.init_identity()) {
        std::cerr << "[CarPlaySession] WARNING: crypto identity init failed" << std::endl;
    }

    // Set up crypto callbacks
    crypto_.set_pin_callback([this](const std::string& pin) {
        // Send PIN to car app for display via OAL control channel
        oal_.send_carplay_pin(pin);
        std::cerr << "[CarPlaySession] CarPlay pairing PIN: " << pin << std::endl;
    });

    crypto_.set_verify_complete_callback([this](bool success) {
        if (success) {
            std::cerr << "[CarPlaySession] pair-verify complete — starting AirPlay streams" << std::endl;
            // Start AirPlay listeners now that crypto is ready
            airplay_.start();
        } else {
            std::cerr << "[CarPlaySession] pair-verify FAILED" << std::endl;
        }
    });

    // Configure AirPlay frame callbacks — these feed OalSession
    airplay_.set_video_callback(
        [this](uint16_t w, uint16_t h, uint32_t pts, uint16_t flags,
               const uint8_t* data, size_t size) {
            on_video_frame(w, h, pts, flags, data, size);
        });

    airplay_.set_audio_callback(
        [this](const uint8_t* pcm, size_t len, uint8_t purpose,
               uint16_t rate, uint8_t ch) {
            on_audio_frame(pcm, len, purpose, rate, ch);
        });

    // Set up RTSP callbacks
    rtsp_.set_request_callback([this](const CarPlayRtsp::Request& req) {
        handle_rtsp_request(req);
    });

    rtsp_.set_connect_callback([this]() {
        std::cerr << "[CarPlaySession] iPhone RTSP connected" << std::endl;
        phone_connected_.store(true);
        oal_.on_phone_connected("iPhone", "iphone");
    });

    rtsp_.set_disconnect_callback([this]() {
        std::cerr << "[CarPlaySession] iPhone RTSP disconnected" << std::endl;
        phone_connected_.store(false);
        audio_started_ = false;
        airplay_.stop();
        oal_.on_phone_disconnected("carplay_disconnected");
    });

    // Start Bonjour advertisement
    bonjour_.start();

    // Start RTSP server on a background thread
    rtsp_thread_ = std::thread([this]() {
        rtsp_.run();
    });

    std::cerr << "[CarPlaySession] started" << std::endl;
}

void CarPlaySession::stop()
{
    if (!running_.exchange(false)) return;

    rtsp_.stop();
    airplay_.stop();
    bonjour_.stop();

    if (rtsp_thread_.joinable()) rtsp_thread_.join();

    phone_connected_.store(false);
    audio_started_ = false;
    std::cerr << "[CarPlaySession] stopped" << std::endl;
}

// ── RTSP Request Dispatch ────────────────────────────────────────

void CarPlaySession::handle_rtsp_request(const CarPlayRtsp::Request& req)
{
    std::cerr << "[CarPlaySession] RTSP " << req.method << " " << req.uri
              << " CSeq=" << req.cseq << std::endl;

    if (req.method == "OPTIONS") {
        handle_options(req);
    } else if (req.method == "ANNOUNCE") {
        handle_announce(req);
    } else if (req.method == "SETUP") {
        handle_setup(req);
    } else if (req.method == "RECORD") {
        handle_record(req);
    } else if (req.method == "GET_PARAMETER") {
        handle_get_parameter(req);
    } else if (req.method == "SET_PARAMETER") {
        handle_set_parameter(req);
    } else if (req.method == "TEARDOWN") {
        handle_teardown(req);
    } else if (req.method == "POST") {
        // POST endpoints for pairing and info
        if (req.uri == "/pair-setup") {
            handle_pair_setup(req);
        } else if (req.uri == "/pair-verify") {
            handle_pair_verify(req);
        } else if (req.uri == "/fp-setup") {
            handle_fp_setup(req);
        } else if (req.uri == "/info") {
            handle_info(req);
        } else {
            std::cerr << "[CarPlaySession] unknown POST URI: " << req.uri << std::endl;
            rtsp_.send_response(req.cseq, 404, "Not Found");
        }
    } else {
        std::cerr << "[CarPlaySession] unknown RTSP method: " << req.method << std::endl;
        rtsp_.send_response(req.cseq, 405, "Method Not Allowed");
    }
}

void CarPlaySession::handle_options(const CarPlayRtsp::Request& req)
{
    rtsp_.send_response(req.cseq, 200, "OK", {
        {"Public", "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, "
                   "OPTIONS, GET_PARAMETER, SET_PARAMETER, POST"}
    });
}

void CarPlaySession::handle_announce(const CarPlayRtsp::Request& req)
{
    // ANNOUNCE carries SDP describing the session parameters.
    // Parse SDP for codec info, screen dimensions, etc.
    if (!req.body.empty()) {
        std::string sdp(req.body.begin(), req.body.end());
        std::cerr << "[CarPlaySession] ANNOUNCE SDP (" << sdp.size() << " bytes)" << std::endl;

        // Extract video parameters from SDP if present
        // m=video ... (port, codec)
        // a=fmtp:... (width, height)
        // For now, use configured dimensions
    }

    rtsp_.send_response(req.cseq, 200, "OK");
}

void CarPlaySession::handle_setup(const CarPlayRtsp::Request& req)
{
    // SETUP configures transport for video and audio streams.
    // iPhone sends Transport header with port info.

    // Extract transport header
    auto it = req.headers.find("Transport");
    std::string transport_response;

    if (it != req.headers.end()) {
        // Echo back the transport with server ports
        transport_response = it->second;
        std::cerr << "[CarPlaySession] SETUP transport: " << transport_response << std::endl;
    }

    std::map<std::string, std::string> headers = {
        {"Session", "1"},
    };
    if (!transport_response.empty()) {
        headers["Transport"] = transport_response;
    }

    rtsp_.send_response(req.cseq, 200, "OK", headers);
}

void CarPlaySession::handle_record(const CarPlayRtsp::Request& req)
{
    // RECORD signals the iPhone to start streaming.
    std::cerr << "[CarPlaySession] iPhone starting media streams" << std::endl;

    // If crypto is verified, AirPlay should already be listening.
    // If it's not started yet (first-time flow), start it now.
    if (!airplay_.is_running() && crypto_.is_encrypted()) {
        airplay_.start();
    }

    rtsp_.send_response(req.cseq, 200, "OK");
}

void CarPlaySession::handle_get_parameter(const CarPlayRtsp::Request& req)
{
    // GET_PARAMETER is used as a keepalive and for querying state.
    rtsp_.send_response(req.cseq, 200, "OK");
}

void CarPlaySession::handle_set_parameter(const CarPlayRtsp::Request& req)
{
    // SET_PARAMETER is used for volume, artwork, metadata, etc.
    auto ct_it = req.headers.find("Content-Type");
    if (ct_it != req.headers.end()) {
        std::cerr << "[CarPlaySession] SET_PARAMETER content-type: " << ct_it->second << std::endl;

        if (ct_it->second == "text/parameters") {
            // Text parameters (e.g., volume)
            std::string params(req.body.begin(), req.body.end());
            std::cerr << "[CarPlaySession] SET_PARAMETER text: " << params << std::endl;

            // Parse volume: "volume: -20.0" → map to 0-100
            if (params.find("volume:") != std::string::npos) {
                // Volume arrives as dB (-144.0 to 0.0)
                // Convert to percentage for the car app
            }
        } else if (ct_it->second == "image/jpeg" || ct_it->second == "image/png") {
            // Album artwork
            std::cerr << "[CarPlaySession] SET_PARAMETER artwork ("
                      << req.body.size() << " bytes)" << std::endl;
        } else if (ct_it->second == "application/x-dmap-tagged") {
            // DMAP metadata (track info)
            std::cerr << "[CarPlaySession] SET_PARAMETER DMAP metadata ("
                      << req.body.size() << " bytes)" << std::endl;
        }
    }

    rtsp_.send_response(req.cseq, 200, "OK");
}

void CarPlaySession::handle_teardown(const CarPlayRtsp::Request& req)
{
    std::cerr << "[CarPlaySession] iPhone session teardown" << std::endl;

    airplay_.stop();
    audio_started_ = false;

    rtsp_.send_response(req.cseq, 200, "OK");
    oal_.on_phone_disconnected("carplay_teardown");
    phone_connected_.store(false);
}

void CarPlaySession::handle_pair_setup(const CarPlayRtsp::Request& req)
{
    // HomeKit pair-setup (first-time pairing with PIN)
    auto response = crypto_.handle_pair_setup(req.body);

    rtsp_.send_response(req.cseq, 200, "OK", {
        {"Content-Type", "application/octet-stream"}
    }, response);
}

void CarPlaySession::handle_pair_verify(const CarPlayRtsp::Request& req)
{
    // HomeKit pair-verify (subsequent reconnections)
    auto response = crypto_.handle_pair_verify(req.body);

    rtsp_.send_response(req.cseq, 200, "OK", {
        {"Content-Type", "application/octet-stream"}
    }, response);
}

void CarPlaySession::handle_fp_setup(const CarPlayRtsp::Request& req)
{
    // FairPlay setup — required even though we don't use DRM.
    // The iPhone insists on completing an FP handshake before streaming.
    //
    // FairPlay setup consists of several message exchanges.
    // The specific response depends on the FP message type.
    //
    // For CarPlay (non-DRM screen mirroring), a minimal FP response
    // is sufficient. Reference: various open-source AirPlay receivers.

    if (req.body.size() < 4) {
        rtsp_.send_response(req.cseq, 400, "Bad Request");
        return;
    }

    uint8_t fp_type = req.body[0];
    std::cerr << "[CarPlaySession] FairPlay setup type=" << (int)fp_type
              << " len=" << req.body.size() << std::endl;

    // FairPlay types:
    //   1: fp-setup message 1 (Hello)
    //   3: fp-setup message 3 (Key exchange)
    //
    // For type 1: respond with a hardcoded response (device-independent)
    // For type 3: respond based on the exchange

    // Minimal FP response — this needs real FairPlay handling
    // to work with actual iPhones, but the structure is correct.
    std::vector<uint8_t> fp_response;

    if (fp_type == 1 && req.body.size() >= 16) {
        // FP type 1 response: 4 bytes
        fp_response = {0x02, 0x00, 0x00, 0x00};
    } else if (fp_type == 3) {
        // FP type 3 response: computed from the exchange
        // This requires the FairPlay SAP implementation
        fp_response = {0x04, 0x00, 0x00, 0x00};
    } else {
        fp_response = {static_cast<uint8_t>(fp_type + 1), 0x00, 0x00, 0x00};
    }

    rtsp_.send_response(req.cseq, 200, "OK", {
        {"Content-Type", "application/octet-stream"}
    }, fp_response);
}

void CarPlaySession::handle_info(const CarPlayRtsp::Request& req)
{
    // Device info exchange — iPhone queries receiver capabilities.
    // Respond with a plist describing our device.

    // Build a minimal binary plist (or use text plist for simplicity)
    // The iPhone expects specific keys:
    //   - deviceID
    //   - features (bitmap)
    //   - model
    //   - name
    //   - sourceVersion
    //   - statusFlags

    std::ostringstream plist;
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
          << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          << "<plist version=\"1.0\">\n"
          << "<dict>\n"
          << "  <key>deviceid</key>\n"
          << "  <string>OpenAutoLink</string>\n"
          << "  <key>features</key>\n"
          << "  <integer>" << CarPlayBonjour::airplay_features() << "</integer>\n"
          << "  <key>model</key>\n"
          << "  <string>OpenAutoLink,1</string>\n"
          << "  <key>name</key>\n"
          << "  <string>" << config_.head_unit_name << "</string>\n"
          << "  <key>sourceVersion</key>\n"
          << "  <string>366.0</string>\n"
          << "  <key>statusFlags</key>\n"
          << "  <integer>4</integer>\n"
          << "  <key>pi</key>\n"
          << "  <string>OpenAutoLink</string>\n"
          << "  <key>vv</key>\n"
          << "  <integer>2</integer>\n"
          << "</dict>\n"
          << "</plist>\n";

    std::string plist_str = plist.str();
    std::vector<uint8_t> body(plist_str.begin(), plist_str.end());

    rtsp_.send_response(req.cseq, 200, "OK", {
        {"Content-Type", "text/x-apple-plist+xml"}
    }, body);
}

// ── Frame Callbacks (AirPlay → OalSession) ───────────────────────

void CarPlaySession::on_video_frame(
    uint16_t width, uint16_t height, uint32_t pts_ms,
    uint16_t flags, const uint8_t* data, size_t size)
{
    // Forward to OalSession — same interface as LiveAasdkSession
    oal_.write_video_frame(width, height, pts_ms, flags, data, size);
}

void CarPlaySession::on_audio_frame(
    const uint8_t* pcm_data, size_t pcm_size,
    uint8_t purpose, uint16_t sample_rate, uint8_t channels)
{
    // Start audio on first frame
    if (!audio_started_) {
        oal_.send_audio_start(purpose, sample_rate, channels);
        audio_started_ = true;
    }

    // Forward to OalSession — same interface as LiveAasdkSession
    oal_.write_audio_frame(pcm_data, pcm_size, purpose, sample_rate, channels);
}

// ── Touch / HID Input ────────────────────────────────────────────

void CarPlaySession::on_touch(int action, float x, float y, int pointer_id)
{
    if (!phone_connected_.load() || !crypto_.is_encrypted()) return;

    auto report = build_hid_touch_report(
        action, x, y, pointer_id,
        static_cast<uint16_t>(config_.video_width),
        static_cast<uint16_t>(config_.video_height));

    send_hid_report(report);
}

void CarPlaySession::on_button(int keycode, bool down)
{
    if (!phone_connected_.load() || !crypto_.is_encrypted()) return;

    auto report = build_hid_key_report(keycode, down);
    send_hid_report(report);
}

std::vector<uint8_t> CarPlaySession::build_hid_touch_report(
    int action, float x, float y, int pointer_id,
    uint16_t screen_width, uint16_t screen_height)
{
    // CarPlay HID digitizer report format:
    //
    // The touch is sent as a USB HID digitizer report.
    // Format varies by CarPlay protocol version, but the common format:
    //
    // [1 byte]  report_id = 0x01 (touch)
    // [1 byte]  touch_state: 0x03=down, 0x04=move, 0x01=up
    // [1 byte]  finger_id
    // [2 bytes] x coordinate (little-endian, 0-16384 range)
    // [2 bytes] y coordinate (little-endian, 0-16384 range)

    std::vector<uint8_t> report(7);

    report[0] = 0x01; // Report ID: touch

    // Map action to HID touch state
    // OAL action: 0=DOWN, 1=UP, 2=MOVE, 5=CANCEL
    switch (action) {
    case 0: report[1] = 0x03; break; // Down
    case 1: report[1] = 0x01; break; // Up
    case 2: report[1] = 0x04; break; // Move
    default: report[1] = 0x01; break; // Default to up
    }

    report[2] = static_cast<uint8_t>(pointer_id & 0xFF);

    // Scale coordinates to HID range (0-16384)
    uint16_t hid_x = static_cast<uint16_t>((x / screen_width) * 16384.0f);
    uint16_t hid_y = static_cast<uint16_t>((y / screen_height) * 16384.0f);

    // Clamp
    if (hid_x > 16384) hid_x = 16384;
    if (hid_y > 16384) hid_y = 16384;

    memcpy(&report[3], &hid_x, 2);
    memcpy(&report[5], &hid_y, 2);

    return report;
}

std::vector<uint8_t> CarPlaySession::build_hid_key_report(int keycode, bool down)
{
    // CarPlay HID key report:
    // [1 byte] report_id = 0x02 (key)
    // [1 byte] key_state: 0x01=down, 0x00=up
    // [2 bytes] keycode (little-endian)

    std::vector<uint8_t> report(4);
    report[0] = 0x02; // Report ID: key
    report[1] = down ? 0x01 : 0x00;

    // Map common keycodes
    // Android KEYCODE_HOME (3) → CarPlay Home (0x0001)
    // Android KEYCODE_VOICE_ASSIST (231) → CarPlay Siri (0x0002)
    // Android KEYCODE_MEDIA_PLAY_PAUSE (85) → CarPlay Play/Pause (0x00B0)
    uint16_t hid_key;
    switch (keycode) {
    case 3:   hid_key = 0x0001; break; // HOME → CarPlay Home
    case 231: hid_key = 0x0002; break; // VOICE_ASSIST → Siri
    case 85:  hid_key = 0x00B0; break; // PLAY_PAUSE → Media Play/Pause
    case 87:  hid_key = 0x00B5; break; // NEXT → Media Next
    case 88:  hid_key = 0x00B6; break; // PREVIOUS → Media Previous
    case 24:  hid_key = 0x00E9; break; // VOLUME_UP
    case 25:  hid_key = 0x00EA; break; // VOLUME_DOWN
    default:  hid_key = static_cast<uint16_t>(keycode); break;
    }

    memcpy(&report[2], &hid_key, 2);
    return report;
}

void CarPlaySession::send_hid_report(const std::vector<uint8_t>& report)
{
    // Encrypt and send via RTSP control channel
    auto encrypted = crypto_.encrypt(report.data(), report.size());
    if (!encrypted.empty()) {
        rtsp_.send_raw(encrypted.data(), encrypted.size());
    }
}

} // namespace openautolink
