#include "openautolink/engine.hpp"

#include <utility>

namespace openautolink {

StubBackendEngine::StubBackendEngine(OutputSink sink, std::string phone_name)
    : session_(std::make_unique<StubAndroidAutoSession>(std::move(sink), std::move(phone_name)))
{
}

StubBackendEngine::StubBackendEngine(std::unique_ptr<IAndroidAutoSession> session)
    : session_(std::move(session))
{
}

void StubBackendEngine::handle_line(const std::string& line)
{
    if(line.empty()) {
        return;
    }
    handle_message(parse_input_message(line));
}

const BackendState& StubBackendEngine::state() const
{
    return session_->state();
}

void StubBackendEngine::handle_message(const ParsedInputMessage& message)
{
    switch(message.type) {
    case InputType::HostCommand:
        if(message.command_id.has_value()) {
            session_->on_host_command(*message.command_id);
        }
        break;
    case InputType::HostPacket:
        handle_host_packet(message);
        break;
    case InputType::Touch:
        session_->on_touch(message);
        break;
    case InputType::AudioInput:
        session_->on_audio_input(message);
        break;
    case InputType::Gnss:
        session_->on_gnss(message);
        break;
    case InputType::VehicleData:
        session_->on_vehicle_data(message);
        break;
    case InputType::Unknown:
        break;
    }
}

void StubBackendEngine::handle_host_packet(const ParsedInputMessage& message)
{
    if(!message.message_name.has_value()) {
        return;
    }

    if(*message.message_name == "OPEN") {
        session_->on_host_open(message);
        return;
    }

    if(*message.message_name == "BOX_SETTINGS") {
        session_->on_host_box_settings(message);
        return;
    }

    if(*message.message_name == "DISCONNECT_PHONE" || *message.message_name == "CLOSE_DONGLE") {
        session_->on_host_disconnect();
        return;
    }

    if(*message.message_name == "HEARTBEAT") {
        session_->on_heartbeat();
    }
}

} // namespace openautolink
