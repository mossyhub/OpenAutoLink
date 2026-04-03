#include "openautolink/carplay_rtsp.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace openautolink {

CarPlayRtsp::CarPlayRtsp(int port) : port_(port) {}

CarPlayRtsp::~CarPlayRtsp()
{
    stop();
}

void CarPlayRtsp::run()
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "[CarPlayRtsp] socket failed: " << strerror(errno) << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[CarPlayRtsp] bind(" << port_ << ") failed: " << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    if (listen(listen_fd_, 1) < 0) {
        std::cerr << "[CarPlayRtsp] listen failed: " << strerror(errno) << std::endl;
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    running_.store(true);
    std::cerr << "[CarPlayRtsp] listening on port " << port_ << std::endl;

    while (running_.load()) {
        // Accept with poll so we can check running_ periodically
        struct pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 1000); // 1s timeout
        if (ret <= 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int cfd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (cfd < 0) {
            if (running_.load()) {
                std::cerr << "[CarPlayRtsp] accept failed: " << strerror(errno) << std::endl;
            }
            continue;
        }

        // Set TCP_NODELAY for low latency
        int nodelay = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        client_fd_ = cfd;
        client_connected_.store(true);

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::cerr << "[CarPlayRtsp] client connected from " << ip_str << std::endl;

        if (connect_cb_) connect_cb_();

        // Read loop
        std::vector<uint8_t> buf;
        buf.reserve(8192);
        uint8_t read_buf[4096];

        while (running_.load() && client_connected_.load()) {
            struct pollfd rpfd{};
            rpfd.fd = cfd;
            rpfd.events = POLLIN;

            int rret = poll(&rpfd, 1, 500);
            if (rret < 0) {
                std::cerr << "[CarPlayRtsp] poll error: " << strerror(errno) << std::endl;
                break;
            }
            if (rret == 0) continue; // timeout

            if (rpfd.revents & (POLLERR | POLLHUP)) {
                std::cerr << "[CarPlayRtsp] client disconnected (POLLHUP)" << std::endl;
                break;
            }

            ssize_t n = read(cfd, read_buf, sizeof(read_buf));
            if (n <= 0) {
                if (n == 0) {
                    std::cerr << "[CarPlayRtsp] client disconnected (EOF)" << std::endl;
                } else {
                    std::cerr << "[CarPlayRtsp] read error: " << strerror(errno) << std::endl;
                }
                break;
            }

            buf.insert(buf.end(), read_buf, read_buf + n);

            // Parse complete RTSP requests from buffer
            Request req;
            while (parse_request(buf, req)) {
                if (request_cb_) {
                    request_cb_(req);
                }
                req = {};
            }
        }

        client_connected_.store(false);
        close(cfd);
        client_fd_ = -1;

        if (disconnect_cb_) disconnect_cb_();

        std::cerr << "[CarPlayRtsp] client session ended" << std::endl;
    }

    close(listen_fd_);
    listen_fd_ = -1;
    running_.store(false);
}

void CarPlayRtsp::stop()
{
    running_.store(false);
    client_connected_.store(false);

    if (client_fd_ >= 0) {
        shutdown(client_fd_, SHUT_RDWR);
        close(client_fd_);
        client_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

void CarPlayRtsp::send_response(int cseq, int status_code, const std::string& status_text,
                                 const std::map<std::string, std::string>& headers,
                                 const std::vector<uint8_t>& body)
{
    std::ostringstream oss;
    oss << "RTSP/1.0 " << status_code << " " << status_text << "\r\n";
    oss << "CSeq: " << cseq << "\r\n";
    oss << "Server: OpenAutoLink/1.0\r\n";

    for (const auto& [key, value] : headers) {
        oss << key << ": " << value << "\r\n";
    }

    if (!body.empty()) {
        oss << "Content-Length: " << body.size() << "\r\n";
    }

    oss << "\r\n";

    std::string header_str = oss.str();

    std::lock_guard<std::mutex> lock(write_mutex_);
    if (client_fd_ < 0) return;

    // Write header
    const char* ptr = header_str.c_str();
    size_t remaining = header_str.size();
    while (remaining > 0) {
        ssize_t n = write(client_fd_, ptr, remaining);
        if (n <= 0) return;
        ptr += n;
        remaining -= n;
    }

    // Write body
    if (!body.empty()) {
        const uint8_t* bptr = body.data();
        remaining = body.size();
        while (remaining > 0) {
            ssize_t n = write(client_fd_, bptr, remaining);
            if (n <= 0) return;
            bptr += n;
            remaining -= n;
        }
    }
}

bool CarPlayRtsp::send_raw(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (client_fd_ < 0) return false;

    const uint8_t* ptr = data;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(client_fd_, ptr, remaining);
        if (n <= 0) return false;
        ptr += n;
        remaining -= n;
    }
    return true;
}

bool CarPlayRtsp::parse_request(std::vector<uint8_t>& buf, Request& out)
{
    // RTSP request format:
    //   METHOD uri RTSP/1.0\r\n
    //   Header: value\r\n
    //   ...
    //   \r\n
    //   [body of Content-Length bytes]

    size_t offset = 0;
    std::string request_line;

    if (!read_line(buf, offset, request_line)) return false;
    if (request_line.empty()) return false;

    // Parse request line: METHOD URI RTSP/1.0
    auto sp1 = request_line.find(' ');
    if (sp1 == std::string::npos) return false;
    auto sp2 = request_line.find(' ', sp1 + 1);

    out.method = request_line.substr(0, sp1);
    if (sp2 != std::string::npos) {
        out.uri = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    } else {
        out.uri = request_line.substr(sp1 + 1);
    }

    // Parse headers until empty line
    int content_length = 0;
    std::string header_line;
    while (read_line(buf, offset, header_line)) {
        if (header_line.empty()) break; // End of headers

        auto colon = header_line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = header_line.substr(0, colon);
        std::string value = header_line.substr(colon + 1);
        // Trim leading whitespace from value
        auto first_nonspace = value.find_first_not_of(' ');
        if (first_nonspace != std::string::npos) {
            value = value.substr(first_nonspace);
        }

        // Store header
        out.headers[key] = value;

        // Extract well-known headers
        if (key == "CSeq" || key == "cseq") {
            try { out.cseq = std::stoi(value); } catch (...) {}
        } else if (key == "Content-Type" || key == "content-type") {
            out.content_type = value;
        } else if (key == "Content-Length" || key == "content-length") {
            try { content_length = std::stoi(value); } catch (...) {}
        }
    }

    // Check if we have the full body
    if (content_length > 0) {
        if (buf.size() - offset < static_cast<size_t>(content_length)) {
            return false; // Need more data
        }
        out.body.assign(buf.begin() + offset, buf.begin() + offset + content_length);
        offset += content_length;
    }

    // Remove parsed data from buffer
    buf.erase(buf.begin(), buf.begin() + offset);
    return true;
}

bool CarPlayRtsp::read_line(const std::vector<uint8_t>& buf, size_t& offset, std::string& line)
{
    // Find \r\n
    for (size_t i = offset; i + 1 < buf.size(); ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            line.assign(buf.begin() + offset, buf.begin() + i);
            offset = i + 2;
            return true;
        }
    }
    return false;
}

} // namespace openautolink
