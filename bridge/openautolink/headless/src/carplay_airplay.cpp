#include "openautolink/carplay_airplay.hpp"
#include "openautolink/carplay_crypto.hpp"
#include "openautolink/oal_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace openautolink {

CarPlayAirPlay::CarPlayAirPlay(Config config, CarPlayCrypto& crypto)
    : config_(std::move(config)), crypto_(crypto) {}

CarPlayAirPlay::~CarPlayAirPlay()
{
    stop();
}

void CarPlayAirPlay::start()
{
    if (running_.load()) return;
    running_.store(true);

    video_listen_fd_ = create_listen_socket(config_.video_port);
    audio_listen_fd_ = create_listen_socket(config_.audio_port);

    if (video_listen_fd_ < 0 || audio_listen_fd_ < 0) {
        std::cerr << "[CarPlayAirPlay] failed to create listen sockets" << std::endl;
        stop();
        return;
    }

    video_thread_ = std::thread(&CarPlayAirPlay::video_accept_loop, this);
    audio_thread_ = std::thread(&CarPlayAirPlay::audio_accept_loop, this);

    std::cerr << "[CarPlayAirPlay] started — video:" << config_.video_port
              << " audio:" << config_.audio_port << std::endl;
}

void CarPlayAirPlay::stop()
{
    running_.store(false);

    auto close_fd = [](int& fd) {
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
            fd = -1;
        }
    };

    close_fd(video_client_fd_);
    close_fd(audio_client_fd_);
    close_fd(video_listen_fd_);
    close_fd(audio_listen_fd_);

    if (video_thread_.joinable()) video_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();

    std::cerr << "[CarPlayAirPlay] stopped" << std::endl;
}

int CarPlayAirPlay::create_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[CarPlayAirPlay] socket failed: " << strerror(errno) << std::endl;
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[CarPlayAirPlay] bind(" << port << ") failed: "
                  << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        std::cerr << "[CarPlayAirPlay] listen(" << port << ") failed: "
                  << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    return fd;
}

void CarPlayAirPlay::video_accept_loop()
{
    std::cerr << "[CarPlayAirPlay] video listening on port " << config_.video_port << std::endl;

    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = video_listen_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int cfd = accept(video_listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (cfd < 0) continue;

        int nodelay = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        video_client_fd_ = cfd;
        video_connected_.store(true);
        video_frame_count_ = 0;
        video_pts_ms_ = 0;

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::cerr << "[CarPlayAirPlay] video client connected from " << ip_str << std::endl;

        // Read loop
        uint8_t read_buf[65536];
        std::vector<uint8_t> frame_buf;
        frame_buf.reserve(131072);

        while (running_.load() && video_connected_.load()) {
            struct pollfd rpfd{};
            rpfd.fd = cfd;
            rpfd.events = POLLIN;

            int rret = poll(&rpfd, 1, 500);
            if (rret < 0) break;
            if (rret == 0) continue;
            if (rpfd.revents & (POLLERR | POLLHUP)) break;

            ssize_t n = read(cfd, read_buf, sizeof(read_buf));
            if (n <= 0) break;

            // Accumulate and process encrypted video data
            frame_buf.insert(frame_buf.end(), read_buf, read_buf + n);
            process_video_data(frame_buf.data(), frame_buf.size());

            // Data is consumed inside process_video_data
            // For now, clear the buffer after processing
            // In production, this needs proper framing to handle partial reads
            frame_buf.clear();
        }

        video_connected_.store(false);
        close(cfd);
        video_client_fd_ = -1;
        std::cerr << "[CarPlayAirPlay] video client disconnected" << std::endl;
    }
}

void CarPlayAirPlay::audio_accept_loop()
{
    std::cerr << "[CarPlayAirPlay] audio listening on port " << config_.audio_port << std::endl;

    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = audio_listen_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int cfd = accept(audio_listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (cfd < 0) continue;

        int nodelay = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        audio_client_fd_ = cfd;
        audio_connected_.store(true);
        audio_frame_count_ = 0;

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::cerr << "[CarPlayAirPlay] audio client connected from " << ip_str << std::endl;

        // Read loop
        uint8_t read_buf[8192];
        std::vector<uint8_t> frame_buf;
        frame_buf.reserve(16384);

        while (running_.load() && audio_connected_.load()) {
            struct pollfd rpfd{};
            rpfd.fd = cfd;
            rpfd.events = POLLIN;

            int rret = poll(&rpfd, 1, 500);
            if (rret < 0) break;
            if (rret == 0) continue;
            if (rpfd.revents & (POLLERR | POLLHUP)) break;

            ssize_t n = read(cfd, read_buf, sizeof(read_buf));
            if (n <= 0) break;

            frame_buf.insert(frame_buf.end(), read_buf, read_buf + n);
            process_audio_data(frame_buf.data(), frame_buf.size());
            frame_buf.clear();
        }

        audio_connected_.store(false);
        close(cfd);
        audio_client_fd_ = -1;
        std::cerr << "[CarPlayAirPlay] audio client disconnected" << std::endl;
    }
}

void CarPlayAirPlay::process_video_data(const uint8_t* data, size_t len)
{
    // AirPlay video stream format (after decryption):
    //
    // Each frame is preceded by a small header indicating:
    //   - Payload type (video)
    //   - Timestamp
    //   - Codec data length
    //
    // The actual format is reverse-engineered and varies slightly
    // between iOS versions. This implementation handles the common
    // case seen in CarPlay receivers.
    //
    // After decryption + deframing, we get H.264 NAL units in
    // AVCC format (length-prefixed). Convert to Annex B for the
    // car app's MediaCodec.

    if (!crypto_.is_encrypted()) {
        // If encryption isn't set up yet, this shouldn't happen
        std::cerr << "[CarPlayAirPlay] received video data before encryption established" << std::endl;
        return;
    }

    // Decrypt the frame
    auto decrypted = crypto_.decrypt(data, len);
    if (decrypted.empty()) {
        // Decryption failed — possibly misaligned data
        return;
    }

    // The decrypted payload contains AirPlay-framed H.264 data.
    // AirPlay video frame header (simplified):
    //   [4 bytes] payload_size (big-endian)
    //   [2 bytes] payload_type (0x00 = video)
    //   [2 bytes] reserved
    //   [8 bytes] NTP timestamp
    //   [H.264 data in AVCC format]

    if (decrypted.size() < 16) return;

    // Extract timestamp (convert NTP to milliseconds)
    uint32_t ntp_hi = (static_cast<uint32_t>(decrypted[8]) << 24) |
                      (static_cast<uint32_t>(decrypted[9]) << 16) |
                      (static_cast<uint32_t>(decrypted[10]) << 8) |
                       static_cast<uint32_t>(decrypted[11]);
    video_pts_ms_ = ntp_hi * 1000; // Simplified — proper NTP conversion later

    // Convert AVCC to Annex B
    auto h264_data = avcc_to_annex_b(decrypted.data() + 16, decrypted.size() - 16);
    if (h264_data.empty()) return;

    // Detect if this is a keyframe (IDR) by scanning NAL types
    uint16_t flags = 0;
    for (size_t i = 0; i + 4 < h264_data.size(); ++i) {
        if (h264_data[i] == 0x00 && h264_data[i+1] == 0x00 &&
            h264_data[i+2] == 0x00 && h264_data[i+3] == 0x01) {
            uint8_t nal_type = h264_data[i + 4] & 0x1F;
            if (nal_type == 5) flags |= OalVideoFlags::KEYFRAME;      // IDR
            if (nal_type == 7 || nal_type == 8) flags |= OalVideoFlags::CODEC_CONFIG; // SPS/PPS
        }
    }

    ++video_frame_count_;

    if (video_cb_) {
        video_cb_(config_.video_width, config_.video_height,
                  video_pts_ms_, flags,
                  h264_data.data(), h264_data.size());
    }
}

void CarPlayAirPlay::process_audio_data(const uint8_t* data, size_t len)
{
    if (!crypto_.is_encrypted()) return;

    auto decrypted = crypto_.decrypt(data, len);
    if (decrypted.empty()) return;

    // AirPlay audio frame header:
    //   [4 bytes] payload_size (big-endian)
    //   [2 bytes] payload_type (audio format indicator)
    //   [2 bytes] reserved
    //   [8 bytes] NTP timestamp
    //   [audio data]

    if (decrypted.size() < 16) return;

    uint16_t audio_type = (static_cast<uint16_t>(decrypted[4]) << 8) |
                           static_cast<uint16_t>(decrypted[5]);

    const uint8_t* audio_data = decrypted.data() + 16;
    size_t audio_len = decrypted.size() - 16;

    std::vector<uint8_t> pcm;
    uint16_t sample_rate = 44100;  // CarPlay default
    uint8_t channels = 2;

    switch (audio_type) {
    case 0x40:  // ALAC
        pcm = decode_alac_to_pcm(audio_data, audio_len, sample_rate, channels);
        break;
    case 0x80:  // AAC-LC
        pcm = decode_aac_to_pcm(audio_data, audio_len, sample_rate, channels);
        break;
    case 0x20:  // PCM (passthrough)
        pcm.assign(audio_data, audio_data + audio_len);
        sample_rate = 48000;
        break;
    default:
        // Unknown audio format — try treating as raw PCM
        pcm.assign(audio_data, audio_data + audio_len);
        break;
    }

    if (pcm.empty()) return;

    ++audio_frame_count_;

    if (audio_cb_) {
        // CarPlay audio is media by default
        audio_cb_(pcm.data(), pcm.size(),
                  OalAudioPurpose::MEDIA, sample_rate, channels);
    }
}

std::vector<uint8_t> CarPlayAirPlay::avcc_to_annex_b(const uint8_t* data, size_t len)
{
    // AVCC format: [4 bytes big-endian length] [NAL unit data] [4 bytes length] [NAL] ...
    // Annex B format: [0x00 0x00 0x00 0x01] [NAL unit data] [start code] [NAL] ...

    std::vector<uint8_t> result;
    result.reserve(len + 32); // Extra space for start codes

    size_t offset = 0;
    while (offset + 4 <= len) {
        uint32_t nal_size = (static_cast<uint32_t>(data[offset]) << 24) |
                            (static_cast<uint32_t>(data[offset + 1]) << 16) |
                            (static_cast<uint32_t>(data[offset + 2]) << 8) |
                             static_cast<uint32_t>(data[offset + 3]);
        offset += 4;

        if (nal_size == 0 || offset + nal_size > len) break;

        // Add Annex B start code
        result.push_back(0x00);
        result.push_back(0x00);
        result.push_back(0x00);
        result.push_back(0x01);

        // Copy NAL data
        result.insert(result.end(), data + offset, data + offset + nal_size);
        offset += nal_size;
    }

    return result;
}

std::vector<uint8_t> CarPlayAirPlay::decode_alac_to_pcm(
    const uint8_t* data, size_t len,
    [[maybe_unused]] int sample_rate,
    [[maybe_unused]] int channels)
{
    // ALAC decoding using libavcodec (ffmpeg).
    // This is a placeholder — full implementation requires linking libavcodec.
    //
    // For now, if libavcodec is not available, we return empty.
    // The bridge build system needs to link -lavcodec -lavutil.
    //
    // TODO: Implement ALAC decode using AVCodecContext with AV_CODEC_ID_ALAC.
    // Alternatively, use Apple's open-source ALAC reference decoder:
    // https://github.com/macosforge/alac

    (void)data;
    (void)len;

    std::cerr << "[CarPlayAirPlay] ALAC decode not yet implemented (need libavcodec)" << std::endl;
    return {};
}

std::vector<uint8_t> CarPlayAirPlay::decode_aac_to_pcm(
    const uint8_t* data, size_t len,
    [[maybe_unused]] int sample_rate,
    [[maybe_unused]] int channels)
{
    // AAC-LC decoding using libavcodec.
    // Placeholder — same as ALAC above.
    //
    // TODO: Implement AAC decode using AVCodecContext with AV_CODEC_ID_AAC.

    (void)data;
    (void)len;

    std::cerr << "[CarPlayAirPlay] AAC decode not yet implemented (need libavcodec)" << std::endl;
    return {};
}

} // namespace openautolink
