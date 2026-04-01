#pragma once

#include <memory>
#include <string>

#include "openautolink/session.hpp"

namespace openautolink {

class StubBackendEngine {
public:
    explicit StubBackendEngine(OutputSink sink, std::string phone_name = "OpenAuto Headless");
    explicit StubBackendEngine(std::unique_ptr<IAndroidAutoSession> session);

    void handle_line(const std::string& line);
    const BackendState& state() const;

private:
    void handle_message(const ParsedInputMessage& message);
    void handle_host_packet(const ParsedInputMessage& message);

    std::unique_ptr<IAndroidAutoSession> session_;
};

} // namespace openautolink
