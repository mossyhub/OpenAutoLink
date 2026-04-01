#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "openautolink/cpc200.hpp"
#include "openautolink/i_car_transport.hpp"
#include "openautolink/headless_config.hpp"

namespace openautolink {

// CPC200 session state machine.
//
// Handles the CPC200 protocol between the car head unit (via FFS USB) and the
// Android Auto headless session.  Owns bootstrap delivery, heartbeat echo,
// and the thread-safe pending-write queue for video/audio frames.
//
// Thread model:
//   - The FFS run() thread calls on_host_packet / on_enable / flush_one_pending.
//   - The aasdk IO thread calls write_video_frame / write_audio_frame / on_phone_*.
//   - pending_writes_ is the only shared state, protected by pending_mutex_.
//
// Write budget per heartbeat:
//   DWC2 FFS can complete exactly TWO blocking writes per heartbeat cycle
//   (one for the echo, one more before the next heartbeat arrives).
//   After bootstrap, flush_one_pending writes ONE queued packet per heartbeat.
//   With fast heartbeats (50ms), this gives ~20 fps throughput.
class CpcSession {
public:
    // Callback to forward control messages to stdout (NDJSON).
    using ControlForwardCallback = std::function<void(const std::string& json_line)>;

    // Takes a copy of config (no dangling reference risk).
    CpcSession(ICarTransport& transport, HeadlessConfig config,
                ICarTransport* audio_transport = nullptr);

    // ── Host-side events (called from FFS run thread) ────────────────

    // Handle a CPC200 packet received from the host (car app).
    void on_host_packet(CpcMessageType type, const uint8_t* payload, size_t len);

    // Called when FFS ENABLE event fires (host reconnected / USB reset).
    void on_enable();

    // Write ONE pending packet to FFS.  Returns true if a packet was written.
    // Called once per heartbeat AFTER the echo, from the FFS run thread.
    bool flush_one_pending();
    bool flush_one_audio();  // Flush from audio queue (separate TCP)

    // ── Phone-side lifecycle (called from aasdk IO thread) ───────────

    // Phone connected via WiFi (queues PLUGGED + PHASE).
    void on_phone_connected(int phone_type = 5, bool wifi = true);

    // Phone disconnected (queues UNPLUGGED).
    void on_phone_disconnected();

    // AA channels opened — session is streaming.
    void on_session_active();

    // ── Media writes (called from aasdk IO thread) ───────────────────

    // Queue a VIDEO_DATA packet for FFS delivery.
    void write_video_frame(
        uint32_t width, uint32_t height,
        uint32_t pts_ms, uint32_t encoder_state, uint32_t flags,
        const uint8_t* h264_data, size_t h264_size);

    // Queue an AUDIO_DATA packet (PCM) for FFS delivery.
    void write_audio_frame(
        const uint8_t* pcm_data, size_t pcm_size,
        uint32_t decode_type, float volume, uint32_t audio_type);

    // Queue an AUDIO_DATA command (13-byte, no PCM) for FFS delivery.
    void write_audio_command(
        uint8_t command_id,
        uint32_t decode_type, float volume, uint32_t audio_type);

    // ── Configuration ────────────────────────────────────────────────

    void set_control_forward(ControlForwardCallback cb) { control_forward_ = std::move(cb); }

    // Set the AA session for direct touch/audio/GNSS forwarding.
    void set_aa_session(class LiveAasdkSession* session) { aa_session_ = session; }

    bool bootstrap_complete() const { return bootstrap_sent_ && deferred_bootstrap_.empty(); }
    int heartbeat_count() const { return heartbeat_count_; }
    const HeadlessConfig& config() const { return config_; }

private:
    void queue_bootstrap();
    void send_config_echo();
    std::string build_box_settings_json() const;

    void queue_write(CpcMessageType type, const uint8_t* payload, size_t len);
    void queue_write(CpcMessageType type, const std::vector<uint8_t>& payload);
    void queue_raw(std::vector<uint8_t>&& data);
    void queue_raw_priority(std::vector<uint8_t>&& data);
    void queue_audio_raw(std::vector<uint8_t>&& data);

    ICarTransport& transport_;
    ICarTransport* audio_transport_ = nullptr;  // separate TCP for audio (null = share main)
    HeadlessConfig config_;   // owned copy

    // Session state (FFS thread only — no lock needed)
    bool bootstrap_sent_ = false;
    int heartbeat_count_ = 0;
    bool phone_connected_ = false;
    bool session_active_ = false;
    std::vector<std::vector<uint8_t>> deferred_bootstrap_;

    // Thread-safe pending write queue (aasdk thread → TCP thread)
    std::mutex pending_mutex_;
    std::deque<std::vector<uint8_t>> pending_writes_;
    static constexpr size_t MAX_PENDING = 120;

    // Separate audio write queue (only used when audio_transport_ is set)
    std::mutex audio_mutex_;
    std::deque<std::vector<uint8_t>> audio_writes_;

    // Stats
    uint64_t media_frames_queued_ = 0;
    uint64_t media_frames_written_ = 0;
    uint64_t media_frames_dropped_ = 0;

    ControlForwardCallback control_forward_;
    class LiveAasdkSession* aa_session_ = nullptr;
};

} // namespace openautolink
