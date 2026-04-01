#pragma once

#include <cstdint>
#include "openautolink/cpc200.hpp"

namespace openautolink {

// Abstract transport interface for CPC200 session.
// Implemented by TcpCarTransport (Ethernet).
class ICarTransport {
public:
    virtual ~ICarTransport() = default;

    virtual bool submit_write(const uint8_t* data, size_t len) = 0;
    virtual bool write_packet(CpcMessageType type, const uint8_t* payload, size_t len) = 0;
    virtual bool write_raw(const uint8_t* data, size_t len) = 0;
    virtual bool is_running() const = 0;
};

} // namespace openautolink
