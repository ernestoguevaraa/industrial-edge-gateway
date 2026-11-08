#include "modbus/libmodbus_errors.hpp"

#include <modbus.h>

#include <cerrno>
#include <cstdint>
#include <format>
#include <string>
#include <system_error>

namespace ieg::modbus::detail {
namespace {

ModbusError make_error(ModbusErrorKind kind, std::optional<std::uint8_t> exception_code,
                       std::optional<int> system_errno, std::string detail)
{
    return ModbusError{.kind = kind,
                       .exception_code = exception_code,
                       .system_errno = system_errno,
                       .detail = std::move(detail)};
}

std::string system_message(int error_number)
{
    return std::system_category().message(error_number);
}

bool is_exception_errno(int error_number) noexcept
{
    return error_number > MODBUS_ENOBASE && error_number <= EMBXGTAR;
}

bool is_protocol_errno(int error_number) noexcept
{
    return error_number >= EMBBADCRC && error_number <= EMBBADSLAVE;
}

} // namespace

ModbusError classify_connect_error(int error_number, std::chrono::milliseconds timeout)
{
    if (error_number == EINPROGRESS || error_number == ETIMEDOUT) {
        return make_error(ModbusErrorKind::ConnectionFailed, std::nullopt, error_number,
                          std::format("connect timed out after {} ms", timeout.count()));
    }
    if (error_number == 0) {
        return make_error(ModbusErrorKind::ConnectionFailed, std::nullopt, std::nullopt,
                          "connect failed without an error code");
    }
    // libmodbus also reports name-resolution failures as ECONNREFUSED.
    return make_error(ModbusErrorKind::ConnectionFailed, std::nullopt, error_number,
                      std::format("connect failed: {}", system_message(error_number)));
}

ModbusError classify_read_error(int error_number)
{
    if (is_exception_errno(error_number)) {
        const auto code = static_cast<std::uint8_t>(error_number - MODBUS_ENOBASE);
        return make_error(
            ModbusErrorKind::ModbusException, code, std::nullopt,
            std::format("Modbus exception 0x{:02X}: {}", code, modbus_strerror(error_number)));
    }
    if (is_protocol_errno(error_number)) {
        // EMBBADEXC also covers exception codes >= 12, which libmodbus does not preserve.
        return make_error(ModbusErrorKind::ProtocolError, std::nullopt, std::nullopt,
                          std::format("invalid response: {}", modbus_strerror(error_number)));
    }
    if (error_number == ETIMEDOUT) {
        return make_error(ModbusErrorKind::Timeout, std::nullopt, error_number,
                          "no complete response within the timeout");
    }
    if (error_number == 0) {
        return make_error(ModbusErrorKind::ProtocolError, std::nullopt, std::nullopt,
                          "request failed without an error code");
    }
    return make_error(ModbusErrorKind::ConnectionLost, std::nullopt, error_number,
                      std::format("connection failed: {}", system_message(error_number)));
}

bool closes_connection(const ModbusError& error) noexcept
{
    switch (error.kind) {
    case ModbusErrorKind::ConnectionLost:
    case ModbusErrorKind::Timeout:
    case ModbusErrorKind::ProtocolError:
        return true;
    case ModbusErrorKind::InvalidRequest:
    case ModbusErrorKind::NotConnected:
    case ModbusErrorKind::ConnectionFailed:
    case ModbusErrorKind::ModbusException:
        return false;
    }
    return true;
}

} // namespace ieg::modbus::detail
