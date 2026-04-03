#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace openautolink {

/// Avahi-based mDNS advertisement for CarPlay discovery.
///
/// Publishes two services:
///   _carplay._tcp  (port 5000) — CarPlay RTSP control
///   _airplay._tcp  (port 7000) — AirPlay media streams
///
/// The iPhone scans for _carplay._tcp to find CarPlay accessories.
/// The feature bitmap in the _airplay._tcp TXT record controls
/// which capabilities the iPhone expects.
///
/// Thread model: runs avahi-publish-service as subprocesses.
/// start() is non-blocking. stop() kills the subprocesses.
class CarPlayBonjour {
public:
    struct Config {
        std::string device_name = "OpenAutoLink";
        std::string device_id;       // MAC-like identifier (XX:XX:XX:XX:XX:XX)
        int rtsp_port = 5000;        // CarPlay RTSP control
        int airplay_port = 7000;     // AirPlay media streams
        std::string model = "OpenAutoLink,1";
    };

    explicit CarPlayBonjour(Config config);
    ~CarPlayBonjour();

    CarPlayBonjour(const CarPlayBonjour&) = delete;
    CarPlayBonjour& operator=(const CarPlayBonjour&) = delete;

    /// Start mDNS advertisements. Non-blocking.
    void start();

    /// Stop mDNS advertisements and kill subprocesses.
    void stop();

    bool is_running() const { return running_.load(); }

    /// Build the AirPlay features bitmap.
    /// Controls what the iPhone expects from the receiver.
    static uint64_t airplay_features();

private:
    void publish_carplay_service();
    void publish_airplay_service();

    /// Generate device ID from system MAC if not provided.
    static std::string generate_device_id();

    Config config_;
    std::atomic<bool> running_{false};
    pid_t carplay_pid_ = -1;
    pid_t airplay_pid_ = -1;
};

} // namespace openautolink
