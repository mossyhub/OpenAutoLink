/**
 * openautolink-relay — TCP relay bridge for direct AA mode.
 *
 * Architecture:
 *   App ──outbound TCP:5291──▶ Relay ◀──TCP:5277── Phone
 *   App ──outbound TCP:5288──▶ Control (JSON lines signaling + diagnostics)
 *
 * The relay does ZERO protocol processing. It splices raw bytes between
 * the app's relay socket and the phone's AA socket. aasdk runs inside
 * the app via NDK/JNI — the relay is just a dumb pipe.
 *
 * Control channel signals:
 *   Bridge → App:  hello, relay_ready, relay_disconnected, phone_bt_connected, paired_phones
 *   App → Bridge:  hello, list_paired_phones, switch_phone, forget_phone, app_log, app_telemetry
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

// ─── Defaults ────────────────────────────────────────────────────────

static constexpr int CONTROL_PORT = 5288;
static constexpr int RELAY_PORT = 5291;
static constexpr int PHONE_PORT = 5277;
static constexpr int SPLICE_BUF_SIZE = 65536;
static constexpr int LISTEN_BACKLOG = 1;

// ─── Globals ─────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static std::mutex g_control_mutex;
static int g_control_fd = -1;  // current app control connection

// ─── Utility ─────────────────────────────────────────────────────────

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_tcp_nodelay(int fd) {
    int val = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
}

static int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[relay] socket() failed: " << strerror(errno) << std::endl;
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[relay] bind(" << port << ") failed: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) < 0) {
        std::cerr << "[relay] listen(" << port << ") failed: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    return fd;
}

static int accept_one(int listen_fd) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    int fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
    if (fd >= 0) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::cerr << "[relay] accepted connection from " << ip
                  << ":" << ntohs(peer.sin_port) << " (fd=" << fd << ")" << std::endl;
        set_tcp_nodelay(fd);
    }
    return fd;
}

// ─── JSON helpers (minimal, no deps) ─────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

/** Extract a string value for a given key from a JSON line (minimal parser). */
static std::string json_extract_string(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};

    // Skip whitespace
    pos = json.find_first_not_of(" \t", pos + 1);
    if (pos == std::string::npos || json[pos] != '"') return {};

    // Extract until closing quote (handle escapes)
    std::string result;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            result += json[++i];
        } else if (json[i] == '"') {
            break;
        } else {
            result += json[i];
        }
    }
    return result;
}

// ─── Control channel ─────────────────────────────────────────────────

static void send_control_line(const std::string& json_line) {
    std::lock_guard<std::mutex> lock(g_control_mutex);
    if (g_control_fd < 0) return;
    std::string msg = json_line + "\n";
    // Best-effort write — control channel is non-critical
    [[maybe_unused]] auto _ = write(g_control_fd, msg.data(), msg.size());
}

static void send_hello() {
    send_control_line(R"({"type":"hello","name":"OpenAutoLink Relay","version":1,"relay_port":5291})");
}

static void send_relay_ready() {
    send_control_line(R"({"type":"relay_ready"})");
}

static void send_relay_disconnected(const std::string& reason) {
    std::ostringstream oss;
    oss << R"({"type":"relay_disconnected","reason":")" << json_escape(reason) << R"("})";
    send_control_line(oss.str());
}

static void send_phone_bt_connected(const std::string& phone_name) {
    std::ostringstream oss;
    oss << R"({"type":"phone_bt_connected","phone_name":")" << json_escape(phone_name) << R"("})";
    send_control_line(oss.str());
}

// ─── Phone management (bluetoothctl integration) ─────────────────────

static bool validate_mac(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (size_t i = 0; i < 17; ++i) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
        } else {
            char c = mac[i];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
                return false;
        }
    }
    return true;
}

static void send_error(int code, const std::string& message) {
    std::ostringstream oss;
    oss << R"({"type":"error","code":)" << code
        << R"(,"message":")" << json_escape(message) << R"("})";
    send_control_line(oss.str());
}

static void handle_list_paired_phones() {
    std::cerr << "[relay] app requested paired phones list" << std::endl;

    FILE* pipe = popen("bluetoothctl devices Paired 2>/dev/null", "r");
    if (!pipe) {
        send_control_line(R"({"type":"paired_phones","phones":[]})");
        return;
    }

    // Get connected devices for status
    std::string connected_macs;
    FILE* conn_pipe = popen("bluetoothctl devices Connected 2>/dev/null", "r");
    if (conn_pipe) {
        char cbuf[256];
        while (fgets(cbuf, sizeof(cbuf), conn_pipe)) {
            connected_macs += cbuf;
        }
        pclose(conn_pipe);
    }

    std::ostringstream oss;
    oss << R"({"type":"paired_phones","phones":[)";

    char buf[256];
    bool first = true;
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();

        if (line.substr(0, 7) != "Device ") continue;
        if (line.size() < 25) continue;

        std::string mac = line.substr(7, 17);
        std::string name = line.size() > 25 ? line.substr(25) : "";
        bool connected = connected_macs.find(mac) != std::string::npos;

        if (!first) oss << ",";
        first = false;
        oss << R"({"mac":")" << json_escape(mac)
            << R"(","name":")" << json_escape(name)
            << R"(","connected":)" << (connected ? "true" : "false") << "}";
    }
    pclose(pipe);

    oss << "]}";
    send_control_line(oss.str());
    std::cerr << "[relay] sent paired_phones" << std::endl;
}

static void handle_switch_phone(const std::string& json) {
    std::string mac = json_extract_string(json, "mac");
    if (mac.empty() || !validate_mac(mac)) {
        send_error(400, "switch_phone requires a valid mac field");
        return;
    }

    std::cerr << "[relay] switching to phone: " << mac << std::endl;

    // Disconnect all, then connect to requested phone (background)
    std::string cmd = "( "
        "for dev in $(bluetoothctl devices Connected 2>/dev/null | awk '{print $2}'); do "
        "bluetoothctl disconnect $dev 2>/dev/null; "
        "done; "
        "sleep 1; "
        "bluetoothctl connect " + mac + " 2>/dev/null; "
        ") &";
    int ret = system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "[relay] switch_phone: command launch failed" << std::endl;
    }
}

static void handle_forget_phone(const std::string& json) {
    std::string mac = json_extract_string(json, "mac");
    if (mac.empty() || !validate_mac(mac)) {
        send_error(400, "forget_phone requires a valid mac field");
        return;
    }

    std::cerr << "[relay] forgetting phone: " << mac << std::endl;

    std::string cmd = "bluetoothctl disconnect " + mac + " 2>/dev/null; "
                      "bluetoothctl remove " + mac + " 2>/dev/null";
    int ret = system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "[relay] forget_phone: bluetoothctl returned " << ret << std::endl;
    }

    // Send updated paired phones list
    handle_list_paired_phones();
}

/** Process one JSON line from the app's control channel. */
static void handle_control_line(const std::string& line) {
    // Extract type
    std::string type = json_extract_string(line, "type");
    if (type.empty()) return;

    if (type == "hello") {
        std::cerr << "[relay] app hello received" << std::endl;
    } else if (type == "list_paired_phones") {
        handle_list_paired_phones();
    } else if (type == "switch_phone") {
        handle_switch_phone(line);
    } else if (type == "forget_phone") {
        handle_forget_phone(line);
    } else if (type == "app_log") {
        // Forward to stderr for journalctl/SSH viewing
        std::string tag = json_extract_string(line, "tag");
        std::string msg = json_extract_string(line, "msg");
        std::string level = json_extract_string(line, "level");
        std::cerr << "[app:" << level << "] " << tag << ": " << msg << std::endl;
    } else if (type == "app_telemetry") {
        // Forward to stderr
        std::cerr << "[app:telemetry] " << line << std::endl;
    } else if (type == "restart_services") {
        // Restart BT (and optionally wireless) to trigger phone reconnection.
        // Used by "Save & Restart" in the app after settings changes that
        // require a new AA session (resolution, codec, DPI, insets, etc.)
        std::string bt = json_extract_string(line, "bluetooth");
        std::string wifi = json_extract_string(line, "wireless");
        std::cerr << "[relay] restart_services: bt=" << bt << " wifi=" << wifi << std::endl;
        if (bt == "true" || bt == "1") {
            int r = system("systemctl restart openautolink-bt 2>/dev/null");
            std::cerr << "[relay] restarted openautolink-bt (exit=" << r << ")" << std::endl;
        }
        if (wifi == "true" || wifi == "1") {
            int r = system("systemctl restart openautolink-wireless 2>/dev/null");
            std::cerr << "[relay] restarted openautolink-wireless (exit=" << r << ")" << std::endl;
        }
    } else {
        std::cerr << "[relay] unknown control message type: " << type << std::endl;
    }
}

// ─── Splice loop ─────────────────────────────────────────────────────

/**
 * Splice raw bytes between two connected sockets using poll().
 * Returns when either socket closes or errors.
 */
static void splice_loop(int app_fd, int phone_fd) {
    set_nonblocking(app_fd);
    set_nonblocking(phone_fd);

    uint8_t buf[SPLICE_BUF_SIZE];
    pollfd fds[2];
    fds[0].fd = app_fd;
    fds[0].events = POLLIN;
    fds[1].fd = phone_fd;
    fds[1].events = POLLIN;

    uint64_t bytes_to_app = 0, bytes_to_phone = 0;
    auto start = std::chrono::steady_clock::now();

    while (g_running) {
        int ret = poll(fds, 2, 1000);  // 1s timeout for shutdown check
        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[relay] poll() error: " << strerror(errno) << std::endl;
            break;
        }
        if (ret == 0) continue;  // timeout

        // Phone → App
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(phone_fd, buf, sizeof(buf));
            if (n <= 0) {
                std::cerr << "[relay] phone socket closed" << std::endl;
                break;
            }
            // Write all to app (blocking write on non-blocking fd — retry short writes)
            size_t written = 0;
            while (written < static_cast<size_t>(n)) {
                ssize_t w = write(app_fd, buf + written, static_cast<size_t>(n) - written);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Poll for writability
                        pollfd wfd{app_fd, POLLOUT, 0};
                        poll(&wfd, 1, 100);
                        continue;
                    }
                    std::cerr << "[relay] app write error: " << strerror(errno) << std::endl;
                    goto done;
                }
                written += static_cast<size_t>(w);
            }
            bytes_to_app += static_cast<uint64_t>(n);
        }

        // App → Phone
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(app_fd, buf, sizeof(buf));
            if (n <= 0) {
                std::cerr << "[relay] app relay socket closed" << std::endl;
                break;
            }
            size_t written = 0;
            while (written < static_cast<size_t>(n)) {
                ssize_t w = write(phone_fd, buf + written, static_cast<size_t>(n) - written);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        pollfd wfd{phone_fd, POLLOUT, 0};
                        poll(&wfd, 1, 100);
                        continue;
                    }
                    std::cerr << "[relay] phone write error: " << strerror(errno) << std::endl;
                    goto done;
                }
                written += static_cast<size_t>(w);
            }
            bytes_to_phone += static_cast<uint64_t>(n);
        }

        // Check for errors/hangups
        if ((fds[0].revents & (POLLERR | POLLHUP)) || (fds[1].revents & (POLLERR | POLLHUP))) {
            std::cerr << "[relay] socket error/hangup" << std::endl;
            break;
        }
    }

done:
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cerr << "[relay] splice ended — " << elapsed << "s, "
              << (bytes_to_app / 1024) << "KB→app, "
              << (bytes_to_phone / 1024) << "KB→phone" << std::endl;
}

// ─── Control channel reader ──────────────────────────────────────────

/**
 * Read JSON lines from the app's control socket.
 * Runs in its own thread. Returns when socket closes.
 */
static void control_reader_loop(int fd) {
    char buf[4096];
    std::string line_buf;

    while (g_running) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) continue;
            break;
        }
        line_buf.append(buf, static_cast<size_t>(n));

        // Process complete lines
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos);
            line_buf.erase(0, pos + 1);

            // Trim trailing CR
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            handle_control_line(line);
        }
    }

    std::cerr << "[relay] control reader exiting" << std::endl;
}

// ─── Main ────────────────────────────────────────────────────────────

static void signal_handler(int) {
    g_running = false;
}

int main() {
    std::ios::sync_with_stdio(false);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    std::cerr << "[relay] OpenAutoLink Relay starting" << std::endl;
    std::cerr << "[relay] control=" << CONTROL_PORT
              << " relay=" << RELAY_PORT
              << " phone=" << PHONE_PORT << std::endl;

    // Create listening sockets
    int control_listen = create_listen_socket(CONTROL_PORT);
    int relay_listen = create_listen_socket(RELAY_PORT);
    int phone_listen = create_listen_socket(PHONE_PORT);

    if (control_listen < 0 || relay_listen < 0 || phone_listen < 0) {
        std::cerr << "[relay] failed to create listening sockets — exiting" << std::endl;
        return 1;
    }

    std::cerr << "[relay] listening on all ports" << std::endl;

    // ── Main loop: accept connections and splice ─────────────────────
    while (g_running) {
        // Step 1: Wait for app control connection
        std::cerr << "[relay] waiting for app control connection on :" << CONTROL_PORT << std::endl;
        int control_fd = accept_one(control_listen);
        if (control_fd < 0) {
            if (!g_running) break;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_control_mutex);
            g_control_fd = control_fd;
        }

        // Send hello to app
        send_hello();
        std::cerr << "[relay] sent hello to app" << std::endl;

        // Start control reader thread
        std::thread control_thread(control_reader_loop, control_fd);

        // Step 2: Wait for app relay connection
        std::cerr << "[relay] waiting for app relay connection on :" << RELAY_PORT << std::endl;
        int relay_fd = accept_one(relay_listen);
        if (relay_fd < 0) {
            std::lock_guard<std::mutex> lock(g_control_mutex);
            g_control_fd = -1;
            close(control_fd);
            if (control_thread.joinable()) control_thread.join();
            continue;
        }

        std::cerr << "[relay] app relay connected — waiting for phone on :"
                  << PHONE_PORT << std::endl;

        // Step 3: Wait for phone connection
        // Use poll() so we can detect app disconnection while waiting
        pollfd wait_fds[2];
        wait_fds[0].fd = phone_listen;
        wait_fds[0].events = POLLIN;
        wait_fds[1].fd = relay_fd;
        wait_fds[1].events = POLLIN;  // detect early close

        int phone_fd = -1;
        while (g_running && phone_fd < 0) {
            int ret = poll(wait_fds, 2, 1000);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue;

            // App relay socket closed while waiting for phone
            if (wait_fds[1].revents & (POLLERR | POLLHUP | POLLIN)) {
                // Check if it's actually data (shouldn't be) or close
                char tmp;
                ssize_t n = recv(relay_fd, &tmp, 1, MSG_PEEK);
                if (n <= 0) {
                    std::cerr << "[relay] app relay disconnected while waiting for phone" << std::endl;
                    break;
                }
            }

            // Phone connected
            if (wait_fds[0].revents & POLLIN) {
                phone_fd = accept_one(phone_listen);
            }
        }

        if (phone_fd < 0) {
            // App left or shutdown
            close(relay_fd);
            {
                std::lock_guard<std::mutex> lock(g_control_mutex);
                g_control_fd = -1;
            }
            close(control_fd);
            if (control_thread.joinable()) control_thread.join();
            continue;
        }

        // Step 4: Both connected — signal app and start splice
        std::cerr << "[relay] phone connected — starting splice" << std::endl;
        send_phone_bt_connected("phone");  // Phone reached us via BT+WiFi
        send_relay_ready();

        splice_loop(relay_fd, phone_fd);

        // Step 5: Session ended — clean up
        send_relay_disconnected("session_ended");

        close(phone_fd);
        close(relay_fd);
        {
            std::lock_guard<std::mutex> lock(g_control_mutex);
            g_control_fd = -1;
        }
        close(control_fd);

        if (control_thread.joinable()) control_thread.join();

        std::cerr << "[relay] session ended — ready for next connection" << std::endl;
    }

    // Cleanup
    close(control_listen);
    close(relay_listen);
    close(phone_listen);

    std::cerr << "[relay] shutdown complete" << std::endl;
    return 0;
}
