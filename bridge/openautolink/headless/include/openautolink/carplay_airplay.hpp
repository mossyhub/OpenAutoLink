#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openautolink {

class CarPlayCrypto;

/// AirPlay stream receiver for CarPlay.
///
/// Accepts encrypted AirPlay streams from the iPhone:
///   - Video (H.264) on TCP port 7000
///   - Audio (ALAC/AAC-LC/PCM) on TCP port 7001
///
/// Decrypts using session keys from CarPlayCrypto, strips AirPlay
/// framing, and delivers raw codec data via callbacks.
///
/// For video: delivers H.264 NAL units (Annex B format with start codes)
/// For audio: delivers raw PCM (decoded from ALAC/AAC if needed)
///
/// Thread model: two threads (video accept + audio accept).
/// Each blocks until connection + streams data.
class CarPlayAirPlay {
public:
    /// Video frame callback: width, height, pts_ms, flags, data, size
    using VideoFrameCallback = std::function<void(
        uint16_t width, uint16_t height, uint32_t pts_ms,
        uint16_t flags, const uint8_t* data, size_t size)>;

    /// Audio frame callback: pcm_data, pcm_size, purpose, sample_rate, channels
    using AudioFrameCallback = std::function<void(
        const uint8_t* pcm_data, size_t pcm_size,
        uint8_t purpose, uint16_t sample_rate, uint8_t channels)>;

    struct Config {
        int video_port = 7000;
        int audio_port = 7001;
        uint16_t video_width = 1920;
        uint16_t video_height = 1080;
    };

    explicit CarPlayAirPlay(Config config, CarPlayCrypto& crypto);
    ~CarPlayAirPlay();

    CarPlayAirPlay(const CarPlayAirPlay&) = delete;
    CarPlayAirPlay& operator=(const CarPlayAirPlay&) = delete;

    void set_video_callback(VideoFrameCallback cb) { video_cb_ = std::move(cb); }
    void set_audio_callback(AudioFrameCallback cb) { audio_cb_ = std::move(cb); }

    /// Start listening for AirPlay streams. Non-blocking (spawns threads).
    void start();

    /// Stop all streams and close connections.
    void stop();

    bool is_running() const { return running_.load(); }
    bool has_video_client() const { return video_connected_.load(); }
    bool has_audio_client() const { return audio_connected_.load(); }

private:
    void video_accept_loop();
    void audio_accept_loop();

    /// Process received encrypted video data.
    /// Decrypts and extracts H.264 NAL units, calls video callback.
    void process_video_data(const uint8_t* data, size_t len);

    /// Process received encrypted audio data.
    /// Decrypts and decodes ALAC/AAC to PCM, calls audio callback.
    void process_audio_data(const uint8_t* data, size_t len);

    /// Create a listening TCP socket on the given port.
    static int create_listen_socket(int port);

    /// Convert AirPlay H.264 AVCC format to Annex B (add start codes).
    static std::vector<uint8_t> avcc_to_annex_b(const uint8_t* data, size_t len);

    /// Decode ALAC audio to PCM.
    /// Returns PCM samples in interleaved int16_t format.
    static std::vector<uint8_t> decode_alac_to_pcm(const uint8_t* data, size_t len,
                                                     int sample_rate, int channels);

    /// Decode AAC-LC audio to PCM.
    static std::vector<uint8_t> decode_aac_to_pcm(const uint8_t* data, size_t len,
                                                    int sample_rate, int channels);

    Config config_;
    CarPlayCrypto& crypto_;

    std::atomic<bool> running_{false};
    std::atomic<bool> video_connected_{false};
    std::atomic<bool> audio_connected_{false};

    int video_listen_fd_ = -1;
    int audio_listen_fd_ = -1;
    int video_client_fd_ = -1;
    int audio_client_fd_ = -1;

    std::thread video_thread_;
    std::thread audio_thread_;

    VideoFrameCallback video_cb_;
    AudioFrameCallback audio_cb_;

    // Video state
    uint32_t video_frame_count_ = 0;
    uint32_t video_pts_ms_ = 0;

    // Audio state
    uint32_t audio_frame_count_ = 0;
};

} // namespace openautolink
