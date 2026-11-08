#include "ieg/modbus/libmodbus_client.hpp"

#include "modbus/libmodbus_errors.hpp"

#include <modbus.h>

#include <cerrno>
#include <cstddef>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace ieg::modbus {
namespace {

constexpr auto kMaxTimeout = std::chrono::milliseconds{60'000};

struct ContextDeleter
{
    void operator()(modbus_t* context) const noexcept
    {
        modbus_close(context); // no-op when no socket is open
        modbus_free(context);
    }
};
using ContextPtr = std::unique_ptr<modbus_t, ContextDeleter>;

void validate(const LibModbusClientOptions& options)
{
    std::string errors;
    if (options.host.empty()) {
        errors += "\n - host must not be empty";
    }
    if (options.port == 0) {
        errors += "\n - port must be 1..65535";
    }
    if (options.unit_id == 0) {
        errors += "\n - unit_id must be 1..255";
    }
    if (options.timeout <= std::chrono::milliseconds::zero() || options.timeout > kMaxTimeout) {
        errors += "\n - timeout must be 1..60000 ms";
    }
    if (!errors.empty()) {
        throw std::invalid_argument("invalid LibModbusClient options:" + errors);
    }
}

void check_setting(int rc, const char* what)
{
    if (rc == -1) {
        throw std::invalid_argument(std::format("libmodbus rejected {}", what));
    }
}

ContextPtr make_context(const LibModbusClientOptions& options)
{
    const std::string service = std::to_string(options.port);
    ContextPtr context{modbus_new_tcp_pi(options.host.c_str(), service.c_str())};
    if (!context) {
        throw std::runtime_error(std::format("libmodbus could not create a TCP context for {}:{}",
                                             options.host, options.port));
    }
    const auto timeout_us =
        std::chrono::duration_cast<std::chrono::microseconds>(options.timeout).count();
    const auto seconds = static_cast<std::uint32_t>(timeout_us / 1'000'000);
    const auto micros = static_cast<std::uint32_t>(timeout_us % 1'000'000);
    check_setting(modbus_set_slave(context.get(), options.unit_id), "the unit ID");
    check_setting(modbus_set_response_timeout(context.get(), seconds, micros),
                  "the response timeout");
    // Disabled byte timeout: the whole response must arrive within the response timeout.
    check_setting(modbus_set_byte_timeout(context.get(), 0, 0), "disabling the byte timeout");
    // No hidden reconnects, flushes or sleeps: every failure is returned to the owner.
    check_setting(modbus_set_error_recovery(context.get(), MODBUS_ERROR_RECOVERY_NONE),
                  "the error recovery mode");
    return context;
}

ModbusError not_connected()
{
    return ModbusError{.kind = ModbusErrorKind::NotConnected,
                       .exception_code = std::nullopt,
                       .system_errno = std::nullopt,
                       .detail = "not connected; call connect() first"};
}

ModbusError short_response(int received, std::size_t requested)
{
    return ModbusError{
        .kind = ModbusErrorKind::ProtocolError,
        .exception_code = std::nullopt,
        .system_errno = std::nullopt,
        .detail = std::format("response holds {} of {} requested values", received, requested)};
}

} // namespace

LibModbusClientOptions client_options_from(const config::ModbusDeviceConfig& device)
{
    return LibModbusClientOptions{.host = device.host,
                                  .port = device.port,
                                  .unit_id = device.unit_id,
                                  .timeout = device.timeout};
}

class LibModbusClient::Impl
{
public:
    explicit Impl(LibModbusClientOptions options) : options_(std::move(options))
    {
        validate(options_);
        context_ = make_context(options_);
    }

    ConnectResult connect()
    {
        close(); // explicit reconnect: never keep two sockets
        errno = 0;
        if (modbus_connect(context_.get()) == -1) {
            const int error_number = errno;
            modbus_close(context_.get()); // releases a partially opened socket, if any
            return ConnectResult::failure(
                detail::classify_connect_error(error_number, options_.timeout));
        }
        connected_ = true;
        return ConnectResult::success(std::monostate{});
    }

    [[nodiscard]] bool connected() const noexcept { return connected_; }

    RegisterReadResult read_registers(config::ModbusTable table, std::uint16_t start,
                                      std::span<std::uint16_t> out)
    {
        if (auto invalid = validate_read(table, start, out.size(), false)) {
            return RegisterReadResult::failure(std::move(*invalid));
        }
        if (!connected_) {
            return RegisterReadResult::failure(not_connected());
        }
        const int count = static_cast<int>(out.size()); // <= 125
        errno = 0;
        const int rc = table == config::ModbusTable::HoldingRegisters
                           ? modbus_read_registers(context_.get(), start, count, out.data())
                           : modbus_read_input_registers(context_.get(), start, count, out.data());
        if (auto error = check(rc, out.size())) {
            return RegisterReadResult::failure(std::move(*error));
        }
        return RegisterReadResult::success(
            RegisterBlock{.table = table, .start = start, .values = out});
    }

    BitReadResult read_bits(config::ModbusTable table, std::uint16_t start,
                            std::span<std::uint8_t> out)
    {
        if (auto invalid = validate_read(table, start, out.size(), true)) {
            return BitReadResult::failure(std::move(*invalid));
        }
        if (!connected_) {
            return BitReadResult::failure(not_connected());
        }
        const int count = static_cast<int>(out.size()); // <= 2000
        errno = 0;
        const int rc = table == config::ModbusTable::Coils
                           ? modbus_read_bits(context_.get(), start, count, out.data())
                           : modbus_read_input_bits(context_.get(), start, count, out.data());
        if (auto error = check(rc, out.size())) {
            return BitReadResult::failure(std::move(*error));
        }
        return BitReadResult::success(BitBlock{.table = table, .start = start, .values = out});
    }

    void close() noexcept
    {
        modbus_close(context_.get()); // idempotent: libmodbus only closes an open socket
        connected_ = false;
    }

private:
    // Maps a libmodbus read result; closes the connection when the stream may be out of sync.
    std::optional<ModbusError> check(int rc, std::size_t requested)
    {
        if (rc == -1) {
            auto error = detail::classify_read_error(errno);
            if (detail::closes_connection(error)) {
                close();
            }
            return error;
        }
        if (static_cast<std::size_t>(rc) != requested) {
            close();
            return short_response(rc, requested);
        }
        return std::nullopt;
    }

    LibModbusClientOptions options_;
    ContextPtr context_;
    bool connected_{false};
};

LibModbusClient::LibModbusClient(LibModbusClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{}

LibModbusClient::~LibModbusClient() = default; // ContextDeleter closes the socket

ConnectResult LibModbusClient::connect()
{
    return impl_->connect();
}

bool LibModbusClient::connected() const noexcept
{
    return impl_->connected();
}

RegisterReadResult LibModbusClient::read_registers(config::ModbusTable table, std::uint16_t start,
                                                   std::span<std::uint16_t> out)
{
    return impl_->read_registers(table, start, out);
}

BitReadResult LibModbusClient::read_bits(config::ModbusTable table, std::uint16_t start,
                                         std::span<std::uint8_t> out)
{
    return impl_->read_bits(table, start, out);
}

void LibModbusClient::close() noexcept
{
    impl_->close();
}

} // namespace ieg::modbus
