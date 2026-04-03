#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "openautolink/carplay_airplay.hpp"
#include "openautolink/carplay_bonjour.hpp"
#include "openautolink/carplay_crypto.hpp"
#include "openautolink/carplay_rtsp.hpp"
#include "openautolink/headless_config.hpp"

namespace openautolink {

class OalSession;

/// CarPlay session manager — the CarPlay equivalent of LiveAasdkSession.
///
/// Owns the full CarPlay protocol stack:
///   - Bonjour (mDNS service advertisement)
///   - RTSP (control channel)
///   - Crypto (HomeKit pair-setup/verify)
///   - AirPlay (video/audio stream receiver)
///
/// Feeds decoded frames into OalSession via the same interface
/// that LiveAasdkSession uses: write_video_frame(), write_audio_frame(),
/// on_phone_connected(), etc. The car app never knows the difference.
///
/// Thread model:
///   - RTSP runs on its own thread (accept + control loop)
///   - AirPlay runs two threads (video + audio accept)
///   - Bonjour uses subprocess (avahi-publish-service)
///   - All frame callbacks run on the receiving thread,
///     which is safe because OalSession's write methods are thread-safe
class CarPlaySession {
public:
    /// Construct a CarPlay session connected to the given OalSession.
    CarPlaySession(OalSession& oal, HeadlessConfig config);
    ~CarPlaySession();

    CarPlaySession(const CarPlaySession&) = delete;
    CarPlaySession& operator=(const CarPlaySession&) = delete;

    /// Start all CarPlay services (Bonjour, RTSP, crypto, AirPlay listeners).
    void start();

    /// Stop all services and clean up.
    void stop();

    bool is_running() const { return running_.load(); }

    /// Check if an iPhone is currently connected and streaming.
    bool is_phone_connected() const { return phone_connected_.load(); }

    /// Handle touch input from the car app (OAL → CarPlay HID).
    /// Converts OAL touch coordinates to CarPlay HID digitizer reports
    /// and sends via the encrypted control channel.
    void on_touch(int action, float x, float y, int pointer_id);

    /// Handle button press from the car app.
    /// Maps OAL keycodes to CarPlay HID key events (home, Siri, etc.)
    void on_button(int keycode, bool down);

private:
    /// RTSP request handler — dispatches to pair-setup, pair-verify,
    /// session setup, and streaming control methods.
    void handle_rtsp_request(const CarPlayRtsp::Request& req);

    /// Handle RTSP OPTIONS — respond with supported methods.
    void handle_options(const CarPlayRtsp::Request& req);

    /// Handle RTSP ANNOUNCE — session parameters.
    void handle_announce(const CarPlayRtsp::Request& req);

    /// Handle RTSP SETUP — configure and start streams.
    void handle_setup(const CarPlayRtsp::Request& req);

    /// Handle RTSP RECORD — start streaming.
    void handle_record(const CarPlayRtsp::Request& req);

    /// Handle RTSP GET_PARAMETER / SET_PARAMETER — ongoing control.
    void handle_get_parameter(const CarPlayRtsp::Request& req);
    void handle_set_parameter(const CarPlayRtsp::Request& req);

    /// Handle RTSP TEARDOWN — end session.
    void handle_teardown(const CarPlayRtsp::Request& req);

    /// Handle POST /pair-setup — HomeKit pairing (first time).
    void handle_pair_setup(const CarPlayRtsp::Request& req);

    /// Handle POST /pair-verify — HomeKit verification (subsequent).
    void handle_pair_verify(const CarPlayRtsp::Request& req);

    /// Handle POST /fp-setup — FairPlay setup (required handshake).
    void handle_fp_setup(const CarPlayRtsp::Request& req);

    /// Handle POST /info — device info exchange.
    void handle_info(const CarPlayRtsp::Request& req);

    /// Called when AirPlay delivers a video frame.
    void on_video_frame(uint16_t width, uint16_t height, uint32_t pts_ms,
                        uint16_t flags, const uint8_t* data, size_t size);

    /// Called when AirPlay delivers an audio frame.
    void on_audio_frame(const uint8_t* pcm_data, size_t pcm_size,
                        uint8_t purpose, uint16_t sample_rate, uint8_t channels);

    /// Build CarPlay HID touch report.
    static std::vector<uint8_t> build_hid_touch_report(
        int action, float x, float y, int pointer_id,
        uint16_t screen_width, uint16_t screen_height);

    /// Build CarPlay HID key report.
    static std::vector<uint8_t> build_hid_key_report(int keycode, bool down);

    /// Send HID report via encrypted RTSP channel.
    void send_hid_report(const std::vector<uint8_t>& report);

    OalSession& oal_;
    HeadlessConfig config_;

    CarPlayBonjour bonjour_;
    CarPlayRtsp rtsp_;
    CarPlayCrypto crypto_;
    CarPlayAirPlay airplay_;

    std::thread rtsp_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> phone_connected_{false};

    // Audio state tracking
    bool audio_started_ = false;
};

} // namespace openautolink
