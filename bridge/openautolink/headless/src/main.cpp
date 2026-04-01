#include <iostream>
#include <mutex>
#include <string>

#include <string_view>

#include "openautolink/engine.hpp"
#include "openautolink/cpc200.hpp"
#include "openautolink/tcp_car_transport.hpp"
#include "openautolink/cpc_session.hpp"

#ifdef PI_AA_ENABLE_AASDK_LIVE
#include "openautolink/live_session.hpp"
#endif

namespace {

std::string_view kSessionModeFlag = "--session-mode=";
std::string_view kPhoneNameFlag = "--phone-name=";
std::string_view kTcpPortFlag = "--tcp-port=";
std::string_view kVideoWidthFlag = "--video-width=";
std::string_view kVideoHeightFlag = "--video-height=";
std::string_view kVideoFpsFlag = "--video-fps=";
std::string_view kVideoDpiFlag = "--video-dpi=";
std::string_view kVideoCodecFlag = "--video-codec=";
std::string_view kAaResolutionFlag = "--aa-resolution=";
std::string_view kHeadUnitNameFlag = "--head-unit-name=";
std::string_view kMediaFdFlag = "--media-fd=";
std::string_view kTcpCarPortFlag = "--tcp-car-port=";
std::string_view kUsbFlag = "--usb";
std::string_view kBtMacFlag = "--bt-mac=";

} // namespace

int main(int argc, char* argv[])
{
    std::ios::sync_with_stdio(false);

    auto session_mode = openautolink::SessionMode::Stub;
    std::string phone_name = "OpenAuto Headless";
    [[maybe_unused]] int tcp_port = 5277;
    int video_width = 0, video_height = 0, video_fps = 0, video_dpi = 0;
    int video_codec = 0, aa_resolution = 0;
    int media_fd = -1;  // Binary media pipe fd (passed by Python parent)
    std::string head_unit_name;
    int tcp_car_port = 0;    // TCP port for car app (e.g. 5288)
    bool use_usb = false;    // Wired AA: phone via USB host port
    std::string bt_mac;      // BT MAC override (empty = auto-detect)
    for(int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if(argument.rfind(kSessionModeFlag, 0) == 0) {
            const auto parsed = openautolink::parse_session_mode(argument.substr(kSessionModeFlag.size()));
            if(!parsed.has_value()) {
                std::cerr << "unsupported session mode: " << argument.substr(kSessionModeFlag.size()) << '\n';
                return 2;
            }
            session_mode = *parsed;
            continue;
        }

        if(argument.rfind(kPhoneNameFlag, 0) == 0) {
            phone_name = std::string(argument.substr(kPhoneNameFlag.size()));
            continue;
        }

        if(argument.rfind(kTcpPortFlag, 0) == 0) {
            tcp_port = std::stoi(std::string(argument.substr(kTcpPortFlag.size())));
            continue;
        }
        if(argument.rfind(kVideoWidthFlag, 0) == 0) {
            video_width = std::stoi(std::string(argument.substr(kVideoWidthFlag.size())));
            continue;
        }
        if(argument.rfind(kVideoHeightFlag, 0) == 0) {
            video_height = std::stoi(std::string(argument.substr(kVideoHeightFlag.size())));
            continue;
        }
        if(argument.rfind(kVideoFpsFlag, 0) == 0) {
            video_fps = std::stoi(std::string(argument.substr(kVideoFpsFlag.size())));
            continue;
        }
        if(argument.rfind(kVideoDpiFlag, 0) == 0) {
            video_dpi = std::stoi(std::string(argument.substr(kVideoDpiFlag.size())));
            continue;
        }
        if(argument.rfind(kVideoCodecFlag, 0) == 0) {
            // h264=3, vp9=5, av1=6, h265=7
            auto val = std::string(argument.substr(kVideoCodecFlag.size()));
            if (val == "h264") video_codec = 3;
            else if (val == "vp9") video_codec = 5;
            else if (val == "av1") video_codec = 6;
            else if (val == "h265") video_codec = 7;
            else video_codec = std::stoi(val);
            continue;
        }
        if(argument.rfind(kAaResolutionFlag, 0) == 0) {
            // 480p=1, 720p=2, 1080p=3, 1440p=4, 4k=5
            auto val = std::string(argument.substr(kAaResolutionFlag.size()));
            if (val == "480p") aa_resolution = 1;
            else if (val == "720p") aa_resolution = 2;
            else if (val == "1080p") aa_resolution = 3;
            else if (val == "1440p") aa_resolution = 4;
            else if (val == "4k") aa_resolution = 5;
            else aa_resolution = std::stoi(val);
            continue;
        }
        if(argument.rfind(kHeadUnitNameFlag, 0) == 0) {
            head_unit_name = std::string(argument.substr(kHeadUnitNameFlag.size()));
            continue;
        }
        if(argument.rfind(kMediaFdFlag, 0) == 0) {
            media_fd = std::stoi(std::string(argument.substr(kMediaFdFlag.size())));
            continue;
        }
        if(argument.rfind(kTcpCarPortFlag, 0) == 0) {
            tcp_car_port = std::stoi(std::string(argument.substr(kTcpCarPortFlag.size())));
            continue;
        }
        if(argument == kUsbFlag) {
            use_usb = true;
            continue;
        }
        if(argument.rfind(kBtMacFlag, 0) == 0) {
            bt_mac = std::string(argument.substr(kBtMacFlag.size()));
            continue;
        }
    }

    // Thread-safe output sink — aasdk callbacks run on a worker thread.
    std::mutex output_mutex;
    auto sink = [&output_mutex](const std::string& payload) {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << payload << '\n';
        std::cout.flush();
    };

    // Build config from CLI args (shared by all modes)
    auto build_config = [&]() {
        openautolink::HeadlessConfig c;
        c.tcp_port = static_cast<uint16_t>(tcp_port);
        if (video_width > 0) c.video_width = video_width;
        if (video_height > 0) c.video_height = video_height;
        if (video_fps > 0) c.video_fps = video_fps;
        if (video_dpi > 0) c.video_dpi = video_dpi;
        if (video_codec > 0) c.video_codec = video_codec;
        if (aa_resolution > 0) c.aa_resolution_tier = aa_resolution;
        if (!head_unit_name.empty()) c.head_unit_name = head_unit_name;
        if (media_fd >= 0) c.media_fd = media_fd;
        c.use_usb_host = use_usb;
        c.bt_mac = bt_mac;
        return c;
    };

#ifdef PI_AA_ENABLE_AASDK_LIVE
    // ── TCP car transport mode ───────────────────────────────────────
    // Bridge serves CPC200 over TCP to the car's app.
    // Car app connects via Ethernet (USB NIC or CDC-ECM gadget).
    if (tcp_car_port > 0 && session_mode == openautolink::SessionMode::AasdkLive) {
        std::cerr << "[main] TCP car transport mode: port " << tcp_car_port << std::endl;

        auto config = build_config();
        config.media_fd = -1;

        openautolink::TcpCarTransport tcp_car(tcp_car_port);
        openautolink::TcpCarTransport tcp_audio(tcp_car_port + 1);  // Audio on port+1 (5289)

        // Start UDP discovery responder + mDNS via avahi-publish
        tcp_car.start_discovery();

        openautolink::CpcSession cpc(tcp_car, config, &tcp_audio);

        cpc.set_control_forward([&output_mutex](const std::string& json_line) {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cout << json_line << '\n';
            std::cout.flush();
        });

        auto live_session = std::make_unique<openautolink::LiveAasdkSession>(
            sink, phone_name, config);

        live_session->set_cpc_session(&cpc);
        cpc.set_aa_session(live_session.get());

        // Audio transport: accept connection + flush thread in background
        int audio_port = tcp_car_port + 1;
        std::thread audio_thread([&tcp_audio, &cpc, audio_port]() {
            std::cerr << "[main] audio TCP listening on port " << audio_port << std::endl;
            tcp_audio.run(
                // Audio transport doesn't receive packets from car (one-way: bridge → car)
                [](openautolink::CpcMessageType, const uint8_t*, size_t) {},
                []() { std::cerr << "[main] audio client connected" << std::endl; },
                [&cpc]() -> bool { return cpc.flush_one_audio(); }
            );
        });
        audio_thread.detach();

        std::cerr << "[main] starting TCP car packet loop on port " << tcp_car_port << std::endl;
        tcp_car.run(
            [&cpc](openautolink::CpcMessageType type,
                   const uint8_t* payload, size_t len) {
                cpc.on_host_packet(type, payload, len);
            },
            [&cpc]() {
                cpc.on_enable();
            },
            [&cpc]() -> bool {
                return cpc.flush_one_pending();
            }
        );
        return 0;
    }
#endif

    std::unique_ptr<openautolink::IAndroidAutoSession> session;

#ifdef PI_AA_ENABLE_AASDK_LIVE
    if (session_mode == openautolink::SessionMode::AasdkLive) {
        auto config = build_config();
        session = std::make_unique<openautolink::LiveAasdkSession>(
            sink, phone_name, std::move(config));
    }
#endif

    if (!session) {
        session = openautolink::create_session(session_mode, sink, phone_name);
    }

    openautolink::StubBackendEngine engine(std::move(session));

    std::string line;
    while(std::getline(std::cin, line)) {
        engine.handle_line(line);
    }

    return 0;
}