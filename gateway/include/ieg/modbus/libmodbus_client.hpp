#pragma once

#include "ieg/config/gateway_config.hpp"
#include "ieg/modbus/modbus_client.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

// libmodbus-backed IModbusClient for Modbus TCP. libmodbus is a private implementation detail:
// this header never includes libmodbus headers or exposes its types.
namespace ieg::modbus {

struct LibModbusClientOptions
{
    // Host name or IP address; resolved by connect() (name resolution is not bounded by
    // `timeout`, see below).
    std::string host;
    std::uint16_t port{502};
    // 1..255; sent in the MBAP header of every request.
    std::uint8_t unit_id{1};
    // 1 ms .. 60 s. Bounds the TCP connect and each wait for response data.
    std::chrono::milliseconds timeout{1000};
};

// Maps the validated device configuration (host, port, unit_id, timeout_ms).
[[nodiscard]] LibModbusClientOptions client_options_from(const config::ModbusDeviceConfig& device);

// Implements the IModbusClient contract (single owner thread, no internal threads, no retries).
//
// Timing (libmodbus 3.1.10, Linux): connect() waits at most `timeout` for the TCP handshake per
// resolved address, after name resolution (getaddrinfo itself is not bounded). A read sends the
// request and then waits for the complete response within `timeout` in total: the libmodbus byte
// timeout is disabled, so the response timeout is not restarted per received chunk and a slowly
// trickling server cannot stretch a read beyond `timeout`. libmodbus' own error recovery (automatic
// reconnect, flush, sleep) is disabled.
//
// Shutdown: a blocked call cannot be cancelled from another thread; calling close() or the
// destructor concurrently is a data race. The owner thread should check its stop flag between
// requests; the stop latency is therefore bounded by the current call (see Timing). Keep
// `timeout` short for responsive shutdown.
class LibModbusClient final : public IModbusClient
{
public:
    // Validates the options and allocates the libmodbus context; no network activity.
    // Throws std::invalid_argument for invalid options and std::runtime_error if libmodbus
    // cannot allocate the context.
    explicit LibModbusClient(LibModbusClientOptions options);
    ~LibModbusClient() override;

    LibModbusClient(const LibModbusClient&) = delete;
    LibModbusClient& operator=(const LibModbusClient&) = delete;
    LibModbusClient(LibModbusClient&&) = delete;
    LibModbusClient& operator=(LibModbusClient&&) = delete;

    [[nodiscard]] ConnectResult connect() override;
    [[nodiscard]] bool connected() const noexcept override;
    [[nodiscard]] RegisterReadResult read_registers(config::ModbusTable table, std::uint16_t start,
                                                    std::span<std::uint16_t> out) override;
    [[nodiscard]] BitReadResult read_bits(config::ModbusTable table, std::uint16_t start,
                                          std::span<std::uint8_t> out) override;
    void close() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ieg::modbus
