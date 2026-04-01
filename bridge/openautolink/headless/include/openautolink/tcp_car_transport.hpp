#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "openautolink/cpc200.hpp"
#include "openautolink/i_car_transport.hpp"

namespace openautolink {

// TCP transport for CPC200 protocol.
// Serves CPC200 packets over a TCP socket to the car's app.
// Car app connects via Ethernet (USB NIC or CDC-ECM gadget).
//
// The app connects as TCP client; this is the server.
// Same CPC200 framing (16-byte header + payload) over the TCP stream.
// No write-budget limitation — TCP handles flow control natively.
class TcpCarTransport : public ICarTransport {
public:
    using PacketCallback = std::function<void(CpcMessageType type, const uint8_t* payload, size_t len)>;
    using EnableCallback = std::function<void()>;
    using FlushCallback = std::function<bool()>;

    explicit TcpCarTransport(int port) : port_(port) {}
    ~TcpCarTransport() { stop(); stop_discovery(); }

    TcpCarTransport(const TcpCarTransport&) = delete;
    TcpCarTransport& operator=(const TcpCarTransport&) = delete;

    // Start UDP discovery responder on port+1 (e.g. 5289).
    // Responds to "OALINK_DISCOVER" with "OALINK_HERE:<port>".
    // Also publishes mDNS service via avahi so the bridge is
    // discoverable as "_openautolink._tcp" / "openautolink.local".
    void start_discovery() {
        discovery_running_.store(true);

        // mDNS: publish service via avahi-publish-service (non-blocking subprocess)
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
            "avahi-publish-service OpenAutoLink _openautolink._tcp %d &", port_);
        if (system(cmd) == 0) {
            fprintf(stderr, "[TcpCar] mDNS: published _openautolink._tcp on port %d\n", port_);
        }

        discovery_thread_ = std::thread([this]() {
            int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (udp_fd < 0) {
                fprintf(stderr, "[TcpCar] discovery socket failed: %s\n", strerror(errno));
                return;
            }

            int opt = 1;
            setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(udp_fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port_ + 1);  // discovery port = TCP port + 1

            if (bind(udp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                fprintf(stderr, "[TcpCar] discovery bind(%d) failed: %s\n", port_ + 1, strerror(errno));
                close(udp_fd);
                return;
            }

            fprintf(stderr, "[TcpCar] discovery responder on UDP %d\n", port_ + 1);

            // Set receive timeout so we can check discovery_running_ periodically
            struct timeval tv{2, 0};
            setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            char buf[64];
            while (discovery_running_.load()) {
                struct sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                ssize_t n = recvfrom(udp_fd, buf, sizeof(buf) - 1, 0,
                                     (struct sockaddr*)&from, &from_len);
                if (n <= 0) continue;
                buf[n] = '\0';

                if (strncmp(buf, "OALINK_DISCOVER", 15) == 0) {
                    char resp[64];
                    int resp_len = snprintf(resp, sizeof(resp), "OALINK_HERE:%d", port_);

                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
                    fprintf(stderr, "[TcpCar] discovery request from %s — responding\n", ip_str);

                    sendto(udp_fd, resp, resp_len, 0,
                           (struct sockaddr*)&from, from_len);
                }
            }

            close(udp_fd);
            fprintf(stderr, "[TcpCar] discovery responder stopped\n");
        });
    }

    void stop_discovery() {
        discovery_running_.store(false);
        if (discovery_thread_.joinable()) discovery_thread_.join();
    }

    // Start listening and accept one client. Blocks in run().
    void run(PacketCallback packet_cb, EnableCallback enable_cb, FlushCallback flush_cb) {
        running_.store(true);

        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            fprintf(stderr, "[TcpCar] socket() failed: %s\n", strerror(errno));
            return;
        }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "[TcpCar] bind(%d) failed: %s\n", port_, strerror(errno));
            close(listen_fd_); listen_fd_ = -1;
            return;
        }

        if (listen(listen_fd_, 1) < 0) {
            fprintf(stderr, "[TcpCar] listen() failed: %s\n", strerror(errno));
            close(listen_fd_); listen_fd_ = -1;
            return;
        }

        fprintf(stderr, "[TcpCar] listening on port %d for car app connection\n", port_);

        while (running_.load()) {
            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client < 0) {
                if (running_.load()) fprintf(stderr, "[TcpCar] accept() failed: %s\n", strerror(errno));
                break;
            }

            // Low-latency settings
            int nodelay = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
            fprintf(stderr, "[TcpCar] car app connected from %s:%d\n", ip_str, ntohs(client_addr.sin_port));

            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                client_fd_ = client;
            }

            // Fire enable callback (equivalent of FFS ENABLE)
            if (enable_cb) enable_cb();

            // Start a flush thread that continuously drains pending writes
            std::atomic<bool> flush_running{true};
            std::thread flush_thread([&flush_cb, &flush_running]() {
                while (flush_running.load()) {
                    if (flush_cb && flush_cb()) {
                        // Flushed a frame — keep going without delay
                        continue;
                    }
                    // Nothing to flush — short sleep
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            });

            // Read loop — read CPC200 packets from car app
            uint8_t header_buf[CPC200_HEADER_SIZE];
            while (running_.load()) {
                if (!read_fully(client, header_buf, CPC200_HEADER_SIZE)) {
                    fprintf(stderr, "[TcpCar] client disconnected (header read)\n");
                    break;
                }

                CpcHeader hdr;
                if (!parse_header(header_buf, hdr)) {
                    fprintf(stderr, "[TcpCar] bad header magic/checksum\n");
                    continue;
                }

                auto type = static_cast<CpcMessageType>(hdr.message_type);
                uint32_t payload_len = hdr.payload_length;

                std::vector<uint8_t> payload;
                if (payload_len > 0 && payload_len < 2 * 1024 * 1024) {
                    payload.resize(payload_len);
                    if (!read_fully(client, payload.data(), payload_len)) {
                        fprintf(stderr, "[TcpCar] client disconnected (payload read)\n");
                        break;
                    }
                }

                if (packet_cb) {
                    packet_cb(type, payload.empty() ? nullptr : payload.data(), payload.size());
                }
                // Flush is handled by the flush thread — no need to flush here
            }

            // Stop flush thread
            flush_running.store(false);
            if (flush_thread.joinable()) flush_thread.join();

            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                close(client_fd_);
                client_fd_ = -1;
            }

            fprintf(stderr, "[TcpCar] client session ended, waiting for reconnect\n");

            // Re-listen for next connection
        }

        close(listen_fd_);
        listen_fd_ = -1;
        running_.store(false);
    }

    void stop() {
        running_.store(false);
        // Close listen socket to unblock accept()
        if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (client_fd_ >= 0) { close(client_fd_); client_fd_ = -1; }
    }

    // Write a CPC200 packet (header + payload) — thread-safe.
    bool write_packet(CpcMessageType type, const uint8_t* payload, size_t len) override {
        uint8_t header[CPC200_HEADER_SIZE];
        auto hdr = make_header(type, static_cast<uint32_t>(len));
        pack_header(header, hdr);

        std::lock_guard<std::mutex> lock(write_mutex_);
        if (client_fd_ < 0) return false;

        if (!write_fully(client_fd_, header, CPC200_HEADER_SIZE)) return false;
        if (len > 0 && payload) {
            if (!write_fully(client_fd_, payload, len)) return false;
        }
        return true;
    }

    // Write raw pre-built CPC200 data (header already included) — thread-safe.
    bool write_raw(const uint8_t* data, size_t len) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (client_fd_ < 0) return false;
        return write_fully(client_fd_, data, len);
    }

    // submit_write is the same as write_raw for TCP (no AIO needed).
    bool submit_write(const uint8_t* data, size_t len) override {
        return write_raw(data, len);
    }

    bool is_running() const override { return running_.load(); }

private:
    static bool read_fully(int fd, uint8_t* buf, size_t count) {
        size_t total = 0;
        while (total < count) {
            ssize_t n = ::read(fd, buf + total, count - total);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    static bool write_fully(int fd, const uint8_t* buf, size_t count) {
        size_t total = 0;
        while (total < count) {
            ssize_t n = ::write(fd, buf + total, count - total);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    int port_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    std::mutex write_mutex_;
    std::atomic<bool> running_{false};

    // UDP discovery
    std::thread discovery_thread_;
    std::atomic<bool> discovery_running_{false};
};

} // namespace openautolink
