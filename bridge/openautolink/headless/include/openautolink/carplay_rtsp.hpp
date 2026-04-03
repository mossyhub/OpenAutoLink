#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openautolink {

/// Minimal RTSP server for CarPlay control channel (TCP port 5000).
///
/// CarPlay uses RTSP (Real Time Streaming Protocol) for session negotiation.
/// The flow:
///   1. iPhone connects to TCP:5000
///   2. OPTIONS — capabilities query
///   3. ANNOUNCE / SETUP — session params (crypto, codec, screen size)
///   4. RECORD — start streaming
///   5. GET_PARAMETER / SET_PARAMETER — ongoing control
///   6. TEARDOWN — session end
///
/// This is NOT a standard media RTSP server. CarPlay uses RTSP as a
/// control plane with custom Apple extensions for AirPlay/CarPlay.
///
/// Thread model: single-threaded accept + read loop.
/// run() blocks until stop() is called or client disconnects.
class CarPlayRtsp {
public:
    /// Parsed RTSP request from the iPhone.
    struct Request {
        std::string method;       // OPTIONS, ANNOUNCE, SETUP, RECORD, etc.
        std::string uri;          // rtsp://... or *
        int cseq = 0;            // Sequence number
        std::string content_type;
        std::vector<uint8_t> body;
        std::map<std::string, std::string> headers;
    };

    /// Callback when a complete RTSP request is received.
    using RequestCallback = std::function<void(const Request& req)>;

    /// Callback when the client connects.
    using ConnectCallback = std::function<void()>;

    /// Callback when the client disconnects.
    using DisconnectCallback = std::function<void()>;

    explicit CarPlayRtsp(int port = 5000);
    ~CarPlayRtsp();

    CarPlayRtsp(const CarPlayRtsp&) = delete;
    CarPlayRtsp& operator=(const CarPlayRtsp&) = delete;

    /// Set callbacks for RTSP events.
    void set_request_callback(RequestCallback cb) { request_cb_ = std::move(cb); }
    void set_connect_callback(ConnectCallback cb) { connect_cb_ = std::move(cb); }
    void set_disconnect_callback(DisconnectCallback cb) { disconnect_cb_ = std::move(cb); }

    /// Send an RTSP response to the connected client.
    /// Must be called from the request callback or while client is connected.
    void send_response(int cseq, int status_code, const std::string& status_text,
                       const std::map<std::string, std::string>& headers = {},
                       const std::vector<uint8_t>& body = {});

    /// Send raw bytes to the connected client.
    bool send_raw(const uint8_t* data, size_t len);

    /// Start listening. Blocks until stop() or error.
    void run();

    /// Stop the server and close connections.
    void stop();

    bool is_running() const { return running_.load(); }
    bool is_connected() const { return client_connected_.load(); }

private:
    /// Parse a single RTSP request from the buffer.
    /// Returns true if a complete request was parsed, removing it from buf.
    bool parse_request(std::vector<uint8_t>& buf, Request& out);

    /// Read a line (ending in \r\n) from the buffer, starting at offset.
    /// Returns the line without \r\n, or empty if no complete line.
    static bool read_line(const std::vector<uint8_t>& buf, size_t& offset, std::string& line);

    int port_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> client_connected_{false};
    std::mutex write_mutex_;

    RequestCallback request_cb_;
    ConnectCallback connect_cb_;
    DisconnectCallback disconnect_cb_;
};

} // namespace openautolink
