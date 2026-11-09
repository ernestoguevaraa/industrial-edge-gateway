#pragma once

#include "ieg/modbus/modbus_client.hpp"

#include <chrono>

// Translation of libmodbus failures (errno after a failed call) into ModbusError. Private to the
// libmodbus adapter and its unit tests.
namespace ieg::modbus::detail {

// After a failed modbus_connect(). A connect timeout leaves errno at EINPROGRESS (libmodbus
// 3.1.10) or ETIMEDOUT; it is still a ConnectionFailed.
[[nodiscard]] ModbusError classify_connect_error(int error_number,
                                                 std::chrono::milliseconds timeout);

// After a failed modbus_read_*(): libmodbus encodes Modbus exception codes 1..11 as
// MODBUS_ENOBASE + code, protocol problems as EMB* codes, and OS failures as plain errno.
[[nodiscard]] ModbusError classify_read_error(int error_number);

// Whether the client must close the connection after this failure (the stream may hold a late
// or partial response). Modbus exceptions are complete, valid answers.
[[nodiscard]] bool closes_connection(const ModbusError& error) noexcept;

} // namespace ieg::modbus::detail
