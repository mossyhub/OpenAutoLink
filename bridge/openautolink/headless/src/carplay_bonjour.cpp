#include "openautolink/carplay_bonjour.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace openautolink {

CarPlayBonjour::CarPlayBonjour(Config config)
    : config_(std::move(config))
{
    if (config_.device_id.empty()) {
        config_.device_id = generate_device_id();
    }
}

CarPlayBonjour::~CarPlayBonjour()
{
    stop();
}

void CarPlayBonjour::start()
{
    if (running_.load()) return;
    running_.store(true);

    publish_carplay_service();
    publish_airplay_service();

    std::cerr << "[CarPlayBonjour] started — _carplay._tcp:" << config_.rtsp_port
              << " _airplay._tcp:" << config_.airplay_port << std::endl;
}

void CarPlayBonjour::stop()
{
    if (!running_.exchange(false)) return;

    auto kill_child = [](pid_t& pid) {
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, WNOHANG);
            pid = -1;
        }
    };

    kill_child(carplay_pid_);
    kill_child(airplay_pid_);

    std::cerr << "[CarPlayBonjour] stopped" << std::endl;
}

void CarPlayBonjour::publish_carplay_service()
{
    // Fork avahi-publish-service for _carplay._tcp
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        // avahi-publish-service <name> <type> <port> [TXT records...]
        //
        // CarPlay TXT records:
        //   features — CarPlay feature flags (hex bitmap)
        //   model    — device model string
        //   srcvers  — source version
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", config_.rtsp_port);

        // CarPlay features: basic car display
        // Bit 9 (0x200): CarPlay control
        // Bit 14 (0x4000): supports car display
        std::string features = "features=0x4200";
        std::string model = "model=" + config_.model;
        std::string srcvers = "srcvers=366.0";
        std::string deviceid = "deviceid=" + config_.device_id;

        execlp("avahi-publish-service",
               "avahi-publish-service",
               config_.device_name.c_str(),
               "_carplay._tcp",
               port_str,
               features.c_str(),
               model.c_str(),
               srcvers.c_str(),
               deviceid.c_str(),
               nullptr);
        // If exec fails
        _exit(1);
    } else if (pid > 0) {
        carplay_pid_ = pid;
        std::cerr << "[CarPlayBonjour] _carplay._tcp published (pid " << pid << ")" << std::endl;
    } else {
        std::cerr << "[CarPlayBonjour] fork failed for _carplay._tcp: "
                  << strerror(errno) << std::endl;
    }
}

void CarPlayBonjour::publish_airplay_service()
{
    pid_t pid = fork();
    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", config_.airplay_port);

        // AirPlay features bitmap — controls what iPhone expects.
        // This bitmap must be precise; wrong bits cause silent connection failure.
        //
        // Known working bits for CarPlay receivers:
        //   Bit  0 (0x01): Video
        //   Bit  1 (0x02): Photo
        //   Bit  7 (0x80): Screen mirroring
        //   Bit  9 (0x200): Audio
        //   Bit 11 (0x800): Audio redundant
        //   Bit 14 (0x4000): FairPlay SAPv2.5 (required for handshake)
        //   Bit 17 (0x20000): Supports unified advertiser info
        //   Bit 18 (0x40000): Supports legacy pairing
        //   Bit 26 (0x4000000): Supports AirPlay from cloud
        //   Bit 27 (0x8000000): Supports TLS_PSK
        //   Bit 30 (0x40000000): Supports buffered audio
        //   Bit 32 (0x100000000): Supports CarPlay
        //   Bit 38 (0x4000000000): Supports buffered video
        //   Bit 40 (0x10000000000): Supports ECDH pairing
        //   Bit 41 (0x20000000000): Supports RSA pairing
        //   Bit 46 (0x400000000000): HomeKit pairing
        //   Bit 48 (0x1000000000000): Supports system pairing
        //   Bit 51 (0x8000000000000): Supports CoreUtils pairing
        //
        // Start with a known-working bitmap from reference implementations.
        // This can be refined through testing with real iPhone.
        uint64_t features = airplay_features();
        char features_hex[32];
        snprintf(features_hex, sizeof(features_hex), "0x%" PRIx64, features);

        std::string feat_str = std::string("features=") + features_hex;
        std::string model = "model=" + config_.model;
        std::string deviceid = "deviceid=" + config_.device_id;
        std::string srcvers = "srcvers=366.0";
        std::string pi = "pi=" + config_.device_id;  // pairing identity
        std::string pk = "pk=";  // public key — set during pair-setup
        // Status flags: 0x04 = has paired, 0x00 = never paired
        std::string flags = "flags=0x04";
        std::string vv = "vv=2";  // protocol version

        execlp("avahi-publish-service",
               "avahi-publish-service",
               (config_.device_name + " AirPlay").c_str(),
               "_airplay._tcp",
               port_str,
               feat_str.c_str(),
               model.c_str(),
               deviceid.c_str(),
               srcvers.c_str(),
               pi.c_str(),
               flags.c_str(),
               vv.c_str(),
               nullptr);
        _exit(1);
    } else if (pid > 0) {
        airplay_pid_ = pid;
        std::cerr << "[CarPlayBonjour] _airplay._tcp published (pid " << pid << ")" << std::endl;
    } else {
        std::cerr << "[CarPlayBonjour] fork failed for _airplay._tcp: "
                  << strerror(errno) << std::endl;
    }
}

uint64_t CarPlayBonjour::airplay_features()
{
    // Feature bitmap for a CarPlay head unit receiver.
    // Based on analysis of working open-source implementations.
    //
    // The key bits are:
    //   Bit  0: Video supported
    //   Bit  7: Screen mirroring
    //   Bit  9: Audio
    //   Bit 14: Supports authentication (FairPlay SAPv2.5)
    //   Bit 32: CarPlay
    //   Bit 40: Supports ECDH key exchange
    //   Bit 46: HomeKit pairing
    //   Bit 51: CoreUtils pairing and transient

    uint64_t f = 0;
    f |= (1ULL << 0);   // Video
    f |= (1ULL << 1);   // Photo
    f |= (1ULL << 7);   // Screen mirroring
    f |= (1ULL << 9);   // Audio
    f |= (1ULL << 11);  // Audio redundant
    f |= (1ULL << 14);  // Authentication setup
    f |= (1ULL << 17);  // Unified advertiser info
    f |= (1ULL << 18);  // Legacy pairing
    f |= (1ULL << 27);  // Supports TLS_PSK
    f |= (1ULL << 30);  // Buffered audio
    f |= (1ULL << 32);  // CarPlay
    f |= (1ULL << 38);  // Buffered video
    f |= (1ULL << 40);  // ECDH pairing
    f |= (1ULL << 46);  // HomeKit pairing
    f |= (1ULL << 48);  // System pairing
    f |= (1ULL << 51);  // CoreUtils pairing
    return f;
}

std::string CarPlayBonjour::generate_device_id()
{
    // Try to read MAC from wlan0 (same interface used for hostapd)
    std::ifstream mac_file("/sys/class/net/wlan0/address");
    std::string mac;
    if (mac_file && std::getline(mac_file, mac) && mac.size() >= 17) {
        // Convert to uppercase: aa:bb:cc:dd:ee:ff → AA:BB:CC:DD:EE:FF
        for (auto& c : mac) {
            if (c >= 'a' && c <= 'f') c -= 32;
        }
        return mac.substr(0, 17);
    }

    // Fallback: try eth0
    std::ifstream eth_file("/sys/class/net/eth0/address");
    if (eth_file && std::getline(eth_file, mac) && mac.size() >= 17) {
        for (auto& c : mac) {
            if (c >= 'a' && c <= 'f') c -= 32;
        }
        return mac.substr(0, 17);
    }

    // Last resort: generate a random-ish one
    return "AA:BB:CC:DD:EE:FF";
}

} // namespace openautolink
