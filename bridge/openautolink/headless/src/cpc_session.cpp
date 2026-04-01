#include "openautolink/cpc_session.hpp"
#include "openautolink/live_session.hpp"

#include <cstring>
#include <iostream>
#include <sstream>

namespace openautolink {

CpcSession::CpcSession(ICarTransport& transport, HeadlessConfig config,
                       ICarTransport* audio_transport)
    : transport_(transport), config_(std::move(config)), audio_transport_(audio_transport)
{
}

// ── Host packet dispatch ─────────────────────────────────────────────

void CpcSession::on_host_packet(CpcMessageType type, const uint8_t* payload, size_t len) {
    switch (type) {
    case CpcMessageType::OPEN: {
        // Car app sent OPEN — re-bootstrap but preserve phone state
        bootstrap_sent_ = false;
        deferred_bootstrap_.clear();
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_writes_.clear();
        }
        queue_bootstrap();

        // If phone already connected, re-send PLUGGED immediately after bootstrap
        if (phone_connected_) {
            auto plugged = pack_plugged_payload(5, 1);
            queue_write(CpcMessageType::PLUGGED, plugged);
            auto phase = pack_int32_payload(7);
            queue_write(CpcMessageType::PHASE, phase);
            std::cerr << "[CPC] OPEN received — re-bootstrapped with phone state" << std::endl;
        } else {
            std::cerr << "[CPC] OPEN received — re-bootstrapped" << std::endl;
        }
        // Forward OPEN to Python for config parsing
        if (control_forward_) {
            std::ostringstream oss;
            oss << R"({"type":"host_packet","message_type":1,"payload_hex":")" ;
            for (size_t i = 0; i < len; i++) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", payload[i]);
                oss << hex;
            }
            oss << "\"}";
            control_forward_(oss.str());
        }
        break;
    }
    case CpcMessageType::HEARTBEAT: {
        heartbeat_count_++;
        if (heartbeat_count_ <= 5 || heartbeat_count_ % 500 == 0) {
            std::cerr << "[CPC] heartbeat #" << heartbeat_count_ << std::endl;
        }
        // Submit echo via AIO (non-blocking).
        transport_.write_packet(CpcMessageType::HEARTBEAT_ECHO, nullptr, 0);

        // Submit ONE deferred bootstrap packet via AIO.
        if (!deferred_bootstrap_.empty()) {
            auto pkt = std::move(deferred_bootstrap_.front());
            deferred_bootstrap_.erase(deferred_bootstrap_.begin());
            transport_.write_raw(pkt.data(), pkt.size());
            if (deferred_bootstrap_.empty()) {
                std::cerr << "[CPC] bootstrap complete (" << heartbeat_count_ << " heartbeats)" << std::endl;
            }
        }
        // Media frames are submitted by flush_one_pending() from the run loop.
        break;
    }
    case CpcMessageType::COMMAND: {
        uint32_t cmd_id = 0;
        if (len >= 4) memcpy(&cmd_id, payload, 4);

        // Handle commands that need direct response
        if (cmd_id == static_cast<uint32_t>(CpcCommand::WIFI_ENABLE) ||
            cmd_id == static_cast<uint32_t>(CpcCommand::WIFI_CONNECT)) {
            // These trigger bootstrap if not sent yet
            if (!bootstrap_sent_) queue_bootstrap();
        }

        // Forward COMMAND to Python
        if (control_forward_) {
            std::ostringstream oss;
            oss << R"({"type":"host_command","command_id":)" << cmd_id << "}";
            control_forward_(oss.str());
        }
        // Forward to AA session for handling (e.g., FRAME → VideoFocusIndication)
        if (aa_session_) {
            aa_session_->on_host_command(static_cast<int>(cmd_id));
        }
        break;
    }
    case CpcMessageType::TOUCH: {
        // AA single-touch: same binary format as MULTI_TOUCH payload
        // [action:u32][x:u32][y:u32][flags:u32] (16 bytes LE)
        {
            uint32_t a = 0, tx = 0, ty = 0;
            if (len >= 12) {
                std::memcpy(&a, payload, 4);
                std::memcpy(&tx, payload + 4, 4);
                std::memcpy(&ty, payload + 8, 4);
            }
            std::cerr << "[CPC] TOUCH received: action=" << a << " x=" << tx << " y=" << ty << " len=" << len << std::endl;
        }
        if (aa_session_) {
            aa_session_->forward_touch(payload, len);
        }
        break;
    }
    case CpcMessageType::MULTI_TOUCH: {
        // Forward touch directly to the AA input channel
        if (aa_session_) {
            aa_session_->forward_touch(payload, len);
        }
        break;
    }
    case CpcMessageType::AUDIO_DATA: {
        // Mic input from car → forward directly to AA audio-input channel
        if (aa_session_) {
            aa_session_->forward_audio_input(payload, len);
        }
        break;
    }
    case CpcMessageType::GNSS_DATA: {
        if (control_forward_) {
            std::ostringstream oss;
            oss << R"({"type":"gnss_data","size":)" << len << "}";
            control_forward_(oss.str());
        }
        // Forward GNSS to aasdk sensor channel
        if (aa_session_ && len > 4) {
            aa_session_->on_vehicle_gnss(payload + 4, len - 4);
        }
        break;
    }
    case CpcMessageType::VEHICLE_DATA: {
        // Forward vehicle sensor data to aasdk SensorBatch
        if (aa_session_ && len > 0) {
            std::string json(reinterpret_cast<const char*>(payload), len);
            aa_session_->on_vehicle_data(json);
        }
        break;
    }
    case CpcMessageType::BOX_SETTINGS: {
        std::string json_str(reinterpret_cast<const char*>(payload), len);

        // Forward to stdout for logging
        if (control_forward_) {
            std::ostringstream oss;
            oss << R"({"type":"box_settings","payload":)" << json_str << "}";
            control_forward_(oss.str());
        }

        // Parse AA stream config and apply if changed
        auto extract_str = [&](const std::string& key) -> std::string {
            auto pos = json_str.find("\"" + key + "\"");
            if (pos == std::string::npos) return "";
            pos = json_str.find(':', pos);
            if (pos == std::string::npos) return "";
            auto start = json_str.find('"', pos + 1);
            if (start == std::string::npos) return "";
            auto end = json_str.find('"', start + 1);
            if (end == std::string::npos) return "";
            return json_str.substr(start + 1, end - start - 1);
        };
        auto extract_int = [&](const std::string& key) -> int {
            auto pos = json_str.find("\"" + key + "\"");
            if (pos == std::string::npos) return -1;
            pos = json_str.find(':', pos);
            if (pos == std::string::npos) return -1;
            pos++;
            while (pos < json_str.size() && json_str[pos] == ' ') pos++;
            return std::atoi(json_str.c_str() + pos);
        };

        std::string aa_res = extract_str("aaResolution");
        int aa_fps = extract_int("aaFps");
        std::string aa_codec = extract_str("aaCodec");
        int aa_dpi = extract_int("aaDpi");

        bool config_changed = false;
        if (!aa_res.empty()) {
            int tier = 3; // default 1080p
            if (aa_res == "480p") tier = 1;
            else if (aa_res == "720p") tier = 2;
            else if (aa_res == "1080p") tier = 3;
            else if (aa_res == "1440p") tier = 4;
            else if (aa_res == "4k") tier = 5;
            if (tier != config_.aa_resolution_tier) {
                config_.aa_resolution_tier = tier;
                config_changed = true;
            }
        }
        if (aa_fps > 0 && aa_fps != config_.video_fps) {
            config_.video_fps = aa_fps;
            config_changed = true;
        }
        if (!aa_codec.empty()) {
            int codec = 3; // h264
            if (aa_codec == "h265") codec = 7;
            else if (aa_codec == "vp9") codec = 5;
            if (codec != config_.video_codec) {
                config_.video_codec = codec;
                config_changed = true;
            }
        }
        if (aa_dpi > 0 && aa_dpi != config_.video_dpi) {
            config_.video_dpi = aa_dpi;
            config_changed = true;
        }

        if (config_changed) {
            std::cerr << "[CPC] AA config updated from app: res=" << aa_res
                << " fps=" << aa_fps << " codec=" << aa_codec
                << " dpi=" << aa_dpi << std::endl;

            // Write updated config to env file for persistence across reboots
            std::string env_update = "#!/bin/bash\n";
            if (!aa_res.empty())
                env_update += "sed -i 's/^OAL_AA_RESOLUTION=.*/OAL_AA_RESOLUTION=" + aa_res + "/' /etc/openautolink.env 2>/dev/null\n";
            if (aa_fps > 0)
                env_update += "sed -i 's/^OAL_AA_FPS=.*/OAL_AA_FPS=" + std::to_string(aa_fps) + "/' /etc/openautolink.env 2>/dev/null\n";
            if (!aa_codec.empty())
                env_update += "sed -i 's/^OAL_AA_CODEC=.*/OAL_AA_CODEC=" + aa_codec + "/' /etc/openautolink.env 2>/dev/null\n";
            if (aa_dpi > 0)
                env_update += "sed -i 's/^OAL_AA_DPI=.*/OAL_AA_DPI=" + std::to_string(aa_dpi) + "/' /etc/openautolink.env 2>/dev/null\n";
            system(env_update.c_str());

            // Restart the AA session with new config (phone auto-reconnects)
            if (aa_session_) {
                std::cerr << "[CPC] Restarting AA session with new config..." << std::endl;
                aa_session_->restart_with_config(config_);
                // Notify car app that phone will reconnect
                on_phone_disconnected();
            }
        }

        // Parse infrastructure settings (written to env, may need service restart)
        std::string phone_mode = extract_str("phoneMode");
        std::string wireless_band = extract_str("wirelessBand");
        std::string wireless_country = extract_str("wirelessCountry");
        std::string wireless_ssid = extract_str("wirelessSsid");
        std::string wireless_password = extract_str("wirelessPassword");
        std::string head_unit_name = extract_str("headUnitName");
        std::string bt_mac = extract_str("btMac");
        std::string car_net_mode = extract_str("carNetMode");
        // carNetUdisk is a boolean — extract as int (0/1)
        // JSON true/false → we check for "true" in the string
        auto udisk_pos = json_str.find("\"carNetUdisk\"");
        bool car_net_udisk = true;
        if (udisk_pos != std::string::npos) {
            car_net_udisk = json_str.find("true", udisk_pos) != std::string::npos;
        }

        bool infra_changed = false;
        std::string infra_update;
        auto sed_env = [&](const std::string& key, const std::string& val) {
            if (!val.empty()) {
                infra_update += "sed -i 's/^" + key + "=.*/" + key + "=" + val + "/' /etc/openautolink.env 2>/dev/null\n";
                infra_changed = true;
            }
        };
        sed_env("OAL_PHONE_MODE", phone_mode);
        sed_env("OAL_WIRELESS_BAND", wireless_band);
        sed_env("OAL_WIRELESS_COUNTRY", wireless_country);
        if (!wireless_ssid.empty())
            sed_env("OAL_WIRELESS_SSID", wireless_ssid);
        if (!wireless_password.empty())
            sed_env("OAL_WIRELESS_PASSWORD", wireless_password);
        sed_env("OAL_HEAD_UNIT_NAME", head_unit_name);
        if (!bt_mac.empty())
            sed_env("OAL_BT_MAC", bt_mac);
        sed_env("OAL_CAR_NET_MODE", car_net_mode);
        if (udisk_pos != std::string::npos) {
            infra_update += "sed -i 's/^OAL_CAR_NET_UDISK=.*/OAL_CAR_NET_UDISK=" +
                std::string(car_net_udisk ? "1" : "0") + "/' /etc/openautolink.env 2>/dev/null\n";
            infra_changed = true;
        }

        if (infra_changed) {
            std::cerr << "[CPC] Infrastructure config updated from app — writing to env" << std::endl;
            system(infra_update.c_str());
        }

        // Echo current bridge config back to car app so it can sync its UI
        send_config_echo();
        break;
    }
    case CpcMessageType::SEND_FILE: {
        // Forward to Python (config file writes)
        if (control_forward_) {
            std::ostringstream oss;
            oss << R"({"type":"send_file","size":)" << len << "}";
            control_forward_(oss.str());
        }
        break;
    }
    case CpcMessageType::DISCONNECT_PHONE:
    case CpcMessageType::CLOSE_DONGLE: {
        if (control_forward_) {
            std::ostringstream oss;
            oss << R"({"type":"disconnect","message_type":)" << static_cast<uint32_t>(type) << "}";
            control_forward_(oss.str());
        }
        break;
    }
    default:
        break;
    }
}

// ── Lifecycle callbacks from AA session ──────────────────────────────

void CpcSession::on_enable() {
    // Reset bootstrap state for the new car app connection
    bootstrap_sent_ = false;
    heartbeat_count_ = 0;
    deferred_bootstrap_.clear();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_writes_.clear();
    }

    // Re-queue bootstrap
    queue_bootstrap();

    // If phone was already connected, re-send PLUGGED + PHASE
    // so the car app knows immediately
    if (phone_connected_) {
        auto plugged = pack_plugged_payload(5, 1); // Android AA, WiFi
        queue_write(CpcMessageType::PLUGGED, plugged);
        auto phase = pack_int32_payload(7);
        queue_write(CpcMessageType::PHASE, phase);
        std::cerr << "[CPC] ENABLE — car app reconnected (phone already connected, re-queued PLUGGED+PHASE)" << std::endl;
        // Replay cached SPS/PPS+IDR so the app can sync video immediately
        if (aa_session_) {
            aa_session_->replay_cached_keyframe();
        }
    } else {
        std::cerr << "[CPC] ENABLE — car app connected (waiting for phone)" << std::endl;
    }
}

void CpcSession::on_phone_connected(int phone_type, bool wifi) {
    phone_connected_ = true;
    // Queue PLUGGED + PHASE for delivery via pending writes
    auto plugged = pack_plugged_payload(phone_type, wifi ? 1 : 0);
    queue_write(CpcMessageType::PLUGGED, plugged);
    auto phase = pack_int32_payload(7);
    queue_write(CpcMessageType::PHASE, phase);
    std::cerr << "[CPC] phone connected (queued PLUGGED+PHASE)" << std::endl;
}

void CpcSession::on_phone_disconnected() {
    phone_connected_ = false;
    session_active_ = false;
    queue_write(CpcMessageType::UNPLUGGED, nullptr, 0);
    std::cerr << "[CPC] phone disconnected (queued UNPLUGGED)" << std::endl;
}

void CpcSession::on_session_active() {
    session_active_ = true;
    std::cerr << "[CPC] session active" << std::endl;
}

// ── Video/Audio write (hot path) ─────────────────────────────────────

void CpcSession::write_video_frame(
    uint32_t width, uint32_t height,
    uint32_t pts_ms, uint32_t encoder_state, uint32_t flags,
    const uint8_t* h264_data, size_t h264_size)
{
    // Build full CPC200 VIDEO_DATA packet
    size_t payload_size = CPC200_VIDEO_HEADER_SIZE + h264_size;
    size_t total_size = CPC200_HEADER_SIZE + payload_size;

    std::vector<uint8_t> pkt(total_size);
    CpcHeader h = make_header(CpcMessageType::VIDEO_DATA, static_cast<uint32_t>(payload_size));
    pack_header(pkt.data(), h);
    build_video_payload(pkt.data() + CPC200_HEADER_SIZE,
                        width, height, encoder_state, pts_ms, flags,
                        h264_data, h264_size);

    queue_raw(std::move(pkt));
}

void CpcSession::write_audio_frame(
    const uint8_t* pcm_data, size_t pcm_size,
    uint32_t decode_type, float volume, uint32_t audio_type)
{
    // Audio payload: [decode_type:4][volume:4][audio_type:4][pcm_data]
    size_t payload_size = 12 + pcm_size;
    std::vector<uint8_t> payload(payload_size);
    memcpy(payload.data() + 0, &decode_type, 4);
    memcpy(payload.data() + 4, &volume, 4);
    memcpy(payload.data() + 8, &audio_type, 4);
    if (pcm_size > 0)
        memcpy(payload.data() + 12, pcm_data, pcm_size);

    // Route audio to separate TCP if available, else priority queue on main
    CpcPacket pkt{CpcMessageType::AUDIO_DATA, std::move(payload)};
    if (audio_transport_) {
        queue_audio_raw(pkt.pack());
    } else {
        queue_raw_priority(pkt.pack());
    }
}

void CpcSession::write_audio_command(
    uint8_t command_id,
    uint32_t decode_type, float volume, uint32_t audio_type)
{
    // Audio command payload: [decode_type:4][volume:4][audio_type:4][command:1]
    std::vector<uint8_t> payload(13);
    memcpy(payload.data() + 0, &decode_type, 4);
    memcpy(payload.data() + 4, &volume, 4);
    memcpy(payload.data() + 8, &audio_type, 4);
    payload[12] = command_id;

    CpcPacket pkt{CpcMessageType::AUDIO_DATA, std::move(payload)};
    if (audio_transport_) {
        queue_audio_raw(pkt.pack());
    } else {
        queue_raw_priority(pkt.pack());
    }
}

// ── Bootstrap ────────────────────────────────────────────────────────

void CpcSession::queue_bootstrap() {
    if (bootstrap_sent_) return;
    bootstrap_sent_ = true;

    // STATUS = 0
    CpcPacket status{CpcMessageType::STATUS_VALUE, pack_int32_payload(0)};
    deferred_bootstrap_.push_back(status.pack());

    // SOFTWARE_VERSION
    CpcPacket version{CpcMessageType::SOFTWARE_VERSION, pack_utf8_payload("OpenAutoLink 1.0")};
    deferred_bootstrap_.push_back(version.pack());

    // BOX_SETTINGS (JSON)
    std::string json = build_box_settings_json();
    CpcPacket settings{CpcMessageType::BOX_SETTINGS, pack_json_payload(json)};
    deferred_bootstrap_.push_back(settings.pack());

    // NOTE: PLUGGED is NOT sent in bootstrap — it's sent when the phone actually
    // connects via on_phone_connected().  Sending a premature PLUGGED causes the
    // app to transition to DEVICE_CONNECTED before the phone is ready.

    std::cerr << "[CPC] bootstrap queued (" << deferred_bootstrap_.size() << " packets)" << std::endl;
}

void CpcSession::send_config_echo() {
    // Send the bridge's current effective config back to the car app as a BOX_SETTINGS
    // with a special "bridgeConfig" key so the app can distinguish it from its own settings.
    std::string codec_name;
    switch (config_.video_codec) {
        case 3: codec_name = "h264"; break;
        case 5: codec_name = "vp9"; break;
        case 7: codec_name = "h265"; break;
        default: codec_name = "h264"; break;
    }
    std::string res_name;
    switch (config_.aa_resolution_tier) {
        case 1: res_name = "480p"; break;
        case 2: res_name = "720p"; break;
        case 3: res_name = "1080p"; break;
        case 4: res_name = "1440p"; break;
        case 5: res_name = "4k"; break;
        default: res_name = "1080p"; break;
    }

    std::ostringstream oss;
    oss << "{"
        << R"("bridgeConfig":true,)"
        << R"("aaResolution":")" << res_name << R"(",)"
        << R"("aaCodec":")" << codec_name << R"(",)"
        << R"("aaFps":)" << config_.video_fps << ","
        << R"("aaDpi":)" << config_.video_dpi << ","
        << R"("headUnitName":")" << config_.head_unit_name << R"(",)"
        << R"("leftHandDrive":)" << (config_.left_hand_drive ? "true" : "false") << ","
        << R"("phoneMode":")" << (config_.use_usb_host ? "usb" : "wireless") << R"(",)"
        << R"("phoneConnected":)" << (phone_connected_ ? "true" : "false") << ","
        << R"("sessionActive":)" << (session_active_ ? "true" : "false")
        << "}";

    auto json = oss.str();
    auto payload = pack_json_payload(json);
    queue_write(CpcMessageType::BOX_SETTINGS, payload);
    std::cerr << "[CPC] Config echo sent (" << json.size() << " bytes)" << std::endl;
}

std::string CpcSession::build_box_settings_json() const {
    std::ostringstream oss;
    oss << "{"
        << R"("adapterType":"openautolink",)"
        << R"("adapterName":"OpenAutoLink",)"
        << R"("wifiType":5,)"
        << R"("btMacAddress":"",)"
        << R"("phoneWorkMode":2,)"
        << R"("androidAutoSizeW":)" << config_.video_width << ","
        << R"("androidAutoSizeH":)" << config_.video_height << ","
        << R"("videoFPS":)" << config_.video_fps << ","
        << R"("videoDPI":)" << config_.video_dpi << ","
        << R"("capabilities":{"directVideo":true,"directAudio":true})"
        << "}";
    return oss.str();
}

// ── Pending write queue ──────────────────────────────────────────────

void CpcSession::queue_write(CpcMessageType type, const uint8_t* payload, size_t len) {
    CpcPacket pkt{type, {}};
    if (payload && len > 0) pkt.payload.assign(payload, payload + len);
    queue_raw(pkt.pack());
}

void CpcSession::queue_write(CpcMessageType type, const std::vector<uint8_t>& payload) {
    CpcPacket pkt{type, payload};
    queue_raw(pkt.pack());
}

void CpcSession::queue_raw(std::vector<uint8_t>&& data) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_writes_.push_back(std::move(data));
    media_frames_queued_++;
    // Cap buffer to prevent unbounded growth — drop oldest frames
    while (pending_writes_.size() > MAX_PENDING) {
        pending_writes_.pop_front();
        media_frames_dropped_++;
    }
}

void CpcSession::queue_raw_priority(std::vector<uint8_t>&& data) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_writes_.push_front(std::move(data));
    media_frames_queued_++;
    while (pending_writes_.size() > MAX_PENDING) {
        pending_writes_.pop_back();
        media_frames_dropped_++;
    }
}

void CpcSession::queue_audio_raw(std::vector<uint8_t>&& data) {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    audio_writes_.push_back(std::move(data));
    // Audio queue: smaller cap, drop oldest
    while (audio_writes_.size() > 60) {  // ~1.2s at 50 audio frames/sec
        audio_writes_.pop_front();
    }
}

bool CpcSession::flush_one_audio() {
    if (!audio_transport_) return false;
    std::vector<uint8_t> pkt;
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (audio_writes_.empty()) return false;
        pkt = std::move(audio_writes_.front());
        audio_writes_.pop_front();
    }
    return audio_transport_->submit_write(pkt.data(), pkt.size());
}

bool CpcSession::flush_one_pending() {
    std::vector<uint8_t> pkt;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_writes_.empty()) return false;
        pkt = std::move(pending_writes_.front());
        pending_writes_.pop_front();
    }
    // Submit via AIO (non-blocking).
    bool ok = transport_.submit_write(pkt.data(), pkt.size());
    if (ok) {
        media_frames_written_++;
        if (media_frames_written_ <= 5 || media_frames_written_ % 300 == 0) {
            size_t pending_size;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_size = pending_writes_.size();
            }
            std::cerr << "[CPC] media: written=" << media_frames_written_
                      << " queued=" << media_frames_queued_
                      << " dropped=" << media_frames_dropped_
                      << " pending=" << pending_size
                      << " size=" << pkt.size() << std::endl;
        }
    } else {
        media_frames_dropped_++;
    }
    return ok;
}

} // namespace openautolink
