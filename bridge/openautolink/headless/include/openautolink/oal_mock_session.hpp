#pragma once

/// OAL mock session — generates synthetic video/audio data for testing
/// without needing a real phone or aasdk. Uses OalSession's existing
/// write_video_frame / write_audio_frame methods to feed data through
/// the same pipeline as live sessions.
///
/// Launched by: --session-mode=oal-mock --tcp-car-port=5288
///
/// Generates:
///   - Synthetic H.264 codec config (SPS+PPS) at startup
///   - IDR frames every 2 seconds, P-frames at configured FPS
///   - PCM sine wave audio at 48kHz stereo, 20ms chunks
///   - Simulated phone_connected after app connects
///   - Media metadata and nav state cycling on control channel

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "openautolink/headless_config.hpp"
#include "openautolink/oal_session.hpp"
#include "openautolink/oal_protocol.hpp"

namespace openautolink {

class OalMockSession {
public:
    explicit OalMockSession(OalSession& oal, HeadlessConfig config)
        : oal_(oal), config_(std::move(config)) {}

    ~OalMockSession() { stop(); }

    OalMockSession(const OalMockSession&) = delete;
    OalMockSession& operator=(const OalMockSession&) = delete;

    /// Start mock generation threads. Call after OalSession is set up.
    void start() {
        running_.store(true);
        video_thread_ = std::thread(&OalMockSession::video_loop, this);
        audio_thread_ = std::thread(&OalMockSession::audio_loop, this);
        control_thread_ = std::thread(&OalMockSession::control_loop, this);
    }

    void stop() {
        running_.store(false);
        if (video_thread_.joinable()) video_thread_.join();
        if (audio_thread_.joinable()) audio_thread_.join();
        if (control_thread_.joinable()) control_thread_.join();
    }

    /// Called by the TCP control thread when the app connects.
    void on_app_connected() {
        app_connected_.store(true);
    }

    void on_app_disconnected() {
        app_connected_.store(false);
        phone_announced_.store(false);
    }

    /// Called when a keyframe_request is received from the app.
    /// Re-sends SPS/PPS + IDR immediately so the app can reset its decoder.
    void on_keyframe_request() {
        keyframe_requested_.store(true);
    }

    /// Called when mic audio is received from the app (direction=1).
    /// In echo mode, loops it back as media playback for testing.
    void on_mic_audio(const OalAudioHeader& hdr, const uint8_t* pcm, size_t len) {
        if (!mic_echo_enabled_.load()) return;
        // Echo mic audio back as media playback (direction=0, purpose=MEDIA)
        oal_.write_audio_frame(pcm, len,
            OalAudioPurpose::MEDIA, hdr.sample_rate, hdr.channels);
    }

    void set_mic_echo(bool enabled) { mic_echo_enabled_.store(enabled); }

private:
    // ── Video generation ─────────────────────────────────────────────

    void video_loop() {
        const auto fps = config_.video_fps > 0 ? config_.video_fps : 30;
        const auto width = static_cast<uint16_t>(config_.video_width > 0 ? config_.video_width : 1920);
        const auto height = static_cast<uint16_t>(config_.video_height > 0 ? config_.video_height : 1080);
        const int idr_interval = fps * 2; // IDR every 2 seconds
        const auto frame_us = std::chrono::microseconds(1'000'000 / fps);

        // Minimal H.264 baseline SPS + PPS (Annex B)
        static const uint8_t sps_pps[] = {
            // SPS: start code + NAL type 7
            0x00, 0x00, 0x00, 0x01, 0x67,
            0x42, 0x00, 0x0A, 0xF8, 0x41, 0xA2,
            // PPS: start code + NAL type 8
            0x00, 0x00, 0x00, 0x01, 0x68,
            0xCE, 0x38, 0x80,
        };

        // Minimal IDR slice header
        static const uint8_t idr_slice[] = {
            0x00, 0x00, 0x00, 0x01, 0x65,
            0x88, 0x80, 0x40, 0x00,
        };

        // Minimal P-frame slice
        static const uint8_t p_slice[] = {
            0x00, 0x00, 0x00, 0x01, 0x41,
            0x9A, 0x00, 0x04,
        };

        // Generate a synthetic frame payload with frame counter embedded
        auto make_frame = [](const uint8_t* base, size_t base_len, uint32_t counter) {
            // Append 4 bytes of counter as "payload" to make frames unique
            std::vector<uint8_t> frame(base_len + 64);
            memcpy(frame.data(), base, base_len);
            // Fill rest with pattern based on counter
            for (size_t i = base_len; i < frame.size(); i++) {
                frame[i] = static_cast<uint8_t>((counter + i) & 0xFF);
            }
            return frame;
        };

        uint32_t pts_ms = 0;
        uint32_t frame_count = 0;
        bool config_sent = false;

        while (running_.load()) {
            if (!app_connected_.load() || !phone_announced_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                config_sent = false;
                pts_ms = 0;
                frame_count = 0;
                continue;
            }

            // Send codec config first (SPS+PPS)
            if (!config_sent) {
                oal_.write_video_frame(width, height, 0,
                    OalVideoFlags::CODEC_CONFIG,
                    sps_pps, sizeof(sps_pps));
                config_sent = true;
                std::cerr << "[mock] sent codec config" << std::endl;
            }

            // IDR or P-frame
            bool is_idr = (frame_count % idr_interval == 0);

            // Handle keyframe request — force IDR + re-send config
            if (keyframe_requested_.exchange(false)) {
                oal_.write_video_frame(width, height, pts_ms,
                    OalVideoFlags::CODEC_CONFIG,
                    sps_pps, sizeof(sps_pps));
                is_idr = true;
                std::cerr << "[mock] keyframe requested — re-sent config + IDR" << std::endl;
            }

            uint16_t flags = is_idr ? OalVideoFlags::KEYFRAME : 0;

            auto frame_data = is_idr
                ? make_frame(idr_slice, sizeof(idr_slice), frame_count)
                : make_frame(p_slice, sizeof(p_slice), frame_count);

            oal_.write_video_frame(width, height, pts_ms, flags,
                frame_data.data(), frame_data.size());

            if (frame_count < 3 || frame_count % (fps * 5) == 0) {
                std::cerr << "[mock] video: frame=" << frame_count
                          << " idr=" << is_idr
                          << " pts=" << pts_ms << std::endl;
            }

            pts_ms += 1000 / fps;
            frame_count++;

            std::this_thread::sleep_for(frame_us);
        }
    }

    // ── Audio generation ─────────────────────────────────────────────

    void audio_loop() {
        constexpr int sample_rate = 48000;
        constexpr int channels = 2;
        constexpr int chunk_ms = 20;
        constexpr int samples_per_chunk = sample_rate * chunk_ms / 1000;
        constexpr size_t chunk_bytes = samples_per_chunk * channels * 2; // 16-bit

        std::vector<uint8_t> pcm(chunk_bytes);
        double t = 0.0;
        double freq = 440.0; // A4
        uint32_t chunks_sent = 0;

        // C major scale for variety
        static const double notes[] = {261.6, 293.7, 329.6, 349.2, 392.0, 440.0, 493.9, 523.3};

        while (running_.load()) {
            if (!app_connected_.load() || !phone_announced_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                t = 0.0;
                chunks_sent = 0;
                continue;
            }

            // Generate sine wave
            auto* samples = reinterpret_cast<int16_t*>(pcm.data());
            for (int i = 0; i < samples_per_chunk; i++) {
                double sample_t = t + static_cast<double>(i) / sample_rate;
                auto value = static_cast<int16_t>(8000.0 * std::sin(2.0 * M_PI * freq * sample_t));
                for (int ch = 0; ch < channels; ch++) {
                    samples[i * channels + ch] = value;
                }
            }
            t += static_cast<double>(samples_per_chunk) / sample_rate;

            oal_.write_audio_frame(pcm.data(), pcm.size(),
                OalAudioPurpose::MEDIA, sample_rate, channels);

            chunks_sent++;
            // Change pitch every 5 seconds
            if (chunks_sent % 250 == 0) {
                freq = notes[(chunks_sent / 250) % (sizeof(notes) / sizeof(notes[0]))];
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(chunk_ms));
        }
    }

    // ── Control simulation ───────────────────────────────────────────

    void control_loop() {
        // Simulated track list
        struct Track {
            const char* title;
            const char* artist;
            const char* album;
            int duration_ms;
        };
        static const Track tracks[] = {
            {"Bohemian Rhapsody", "Queen", "A Night at the Opera", 354000},
            {"Hotel California", "Eagles", "Hotel California", 391000},
            {"Stairway to Heaven", "Led Zeppelin", "Led Zeppelin IV", 482000},
            {"Imagine", "John Lennon", "Imagine", 187000},
            {"Comfortably Numb", "Pink Floyd", "The Wall", 382000},
        };
        static const size_t num_tracks = sizeof(tracks) / sizeof(tracks[0]);

        struct Maneuver {
            const char* type;
            int distance_m;
            const char* road;
            int eta_s;
        };
        static const Maneuver maneuvers[] = {
            {"turn_right", 500, "Main St", 420},
            {"turn_left", 200, "Oak Ave", 380},
            {"straight", 1200, "Highway 101", 350},
            {"turn_right", 50, "Elm Dr", 300},
            {"destination", 0, "123 Elm Dr", 0},
        };
        static const size_t num_maneuvers = sizeof(maneuvers) / sizeof(maneuvers[0]);

        // Wait for app to connect
        while (running_.load() && !app_connected_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!running_.load()) return;

        // Simulate phone connecting after 1 second
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_.load()) return;

        oal_.on_phone_connected("Mock Pixel", "android", /*deferred=*/false);
        oal_.send_audio_start(OalAudioPurpose::MEDIA, 48000, 2);
        phone_announced_.store(true);
        std::cerr << "[mock] simulated phone connection" << std::endl;

        // Send initial phone battery
        oal_.send_phone_battery(85, 14400, false);

        size_t track_idx = 0;
        size_t maneuver_idx = 0;
        int tick = 0;

        while (running_.load() && app_connected_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            tick++;

            // Simulate phone battery drain every 30 seconds
            if (tick % 30 == 0) {
                int battery = 85 - (tick / 30) % 70; // cycle 85 → 15
                bool critical = battery <= 15;
                oal_.send_phone_battery(battery, battery * 180, critical);
            }

            // Simulate voice session every 60 seconds (5 second burst)
            if (tick % 60 == 20) {
                oal_.send_voice_session(true);
                std::cerr << "[mock] voice session start" << std::endl;
            }
            if (tick % 60 == 25) {
                oal_.send_voice_session(false);
                std::cerr << "[mock] voice session end" << std::endl;
            }

            // Update media metadata every 15 seconds
            if (tick % 15 == 1) {
                auto& t = tracks[track_idx % num_tracks];
                int pos_ms = ((tick - 1) % 15) * 1000;
                oal_.send_media_metadata(
                    t.title, t.artist, t.album,
                    t.duration_ms, pos_ms, true);
                if (tick % 15 == 1) {
                    std::cerr << "[mock] now playing: " << t.artist << " - " << t.title << std::endl;
                    track_idx++;
                }
            }

            // Update nav state every 10 seconds
            if (tick % 10 == 3) {
                auto& m = maneuvers[maneuver_idx % num_maneuvers];
                oal_.send_nav_state(m.type, m.distance_m, m.road, m.eta_s);
                std::cerr << "[mock] nav: " << m.type << " in " << m.distance_m << "m → " << m.road << std::endl;
                maneuver_idx++;
            }
        }

        // If app disconnected, announce phone disconnect
        if (!app_connected_.load() && phone_announced_.load()) {
            phone_announced_.store(false);
        }
    }

    OalSession& oal_;
    HeadlessConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> app_connected_{false};
    std::atomic<bool> phone_announced_{false};
    std::atomic<bool> keyframe_requested_{false};
    std::atomic<bool> mic_echo_enabled_{false};

    std::thread video_thread_;
    std::thread audio_thread_;
    std::thread control_thread_;
};

} // namespace openautolink
