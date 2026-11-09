#include "ieg/modbus/modbus_client.hpp"

#include <format>

namespace ieg::modbus {

std::string_view to_string(ModbusErrorKind kind) noexcept
{
    switch (kind) {
    case ModbusErrorKind::InvalidRequest:
        return "invalid_request";
    case ModbusErrorKind::NotConnected:
        return "not_connected";
    case ModbusErrorKind::ConnectionFailed:
        return "connection_failed";
    case ModbusErrorKind::ConnectionLost:
        return "connection_lost";
    case ModbusErrorKind::Timeout:
        return "timeout";
    case ModbusErrorKind::ModbusException:
        return "modbus_exception";
    case ModbusErrorKind::ProtocolError:
        return "protocol_error";
    }
    return "unknown";
}

std::optional<ModbusError> validate_read(config::ModbusTable table, std::uint16_t start,
                                         std::size_t quantity, bool bits)
{
    const auto invalid = [](std::string detail) {
        return ModbusError{.kind = ModbusErrorKind::InvalidRequest,
                           .exception_code = std::nullopt,
                           .system_errno = std::nullopt,
                           .detail = std::move(detail)};
    };
    if (config::is_bit_table(table) != bits) {
        return invalid(bits ? "bit reads require table coils or discrete_inputs"
                            : "register reads require table holding_registers or input_registers");
    }
    const std::size_t limit = bits ? kMaxReadBits : kMaxReadRegisters;
    if (quantity == 0 || quantity > limit) {
        return invalid(
            std::format("quantity {} is outside 1..{} for one request", quantity, limit));
    }
    if (static_cast<std::size_t>(start) + quantity > kAddressSpace) {
        return invalid(std::format("addresses {}..{} exceed the last address 65535", start,
                                   static_cast<std::size_t>(start) + quantity - 1));
    }
    return std::nullopt;
}

} // namespace ieg::modbus
