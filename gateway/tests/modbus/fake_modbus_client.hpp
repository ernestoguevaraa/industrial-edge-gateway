#pragma once

#include "ieg/modbus/modbus_client.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace ieg::modbus::testing {

// Scripted IModbusClient for tests of code that drives a Modbus client (e.g. the future poller).
//
// connect() and each read pop the next scripted outcome; without a script, connect() succeeds
// and reads return zeros. It applies the same request validation as the real client, the same
// NotConnected rule, and closes itself after failures that close a real connection.
class FakeModbusClient final : public IModbusClient
{
public:
    struct Request
    {
        config::ModbusTable table{};
        std::uint16_t start{};
        std::size_t quantity{};

        friend bool operator==(const Request&, const Request&) = default;
    };

    // A scripted read outcome: values to return (registers or bits) or an error.
    using Outcome = std::variant<std::vector<std::uint16_t>, ModbusError>;

    void script_connect(const std::optional<ModbusError>& error)
    {
        connect_script_.push_back(error);
    }
    void script_read(Outcome outcome) { read_script_.push_back(std::move(outcome)); }

    [[nodiscard]] ConnectResult connect() override
    {
        ++connect_calls_;
        connected_ = false;
        std::optional<ModbusError> error;
        if (!connect_script_.empty()) {
            error = connect_script_.front();
            connect_script_.pop_front();
        }
        if (error) {
            return ConnectResult::failure(*error);
        }
        connected_ = true;
        return ConnectResult::success(std::monostate{});
    }

    [[nodiscard]] bool connected() const noexcept override { return connected_; }

    [[nodiscard]] RegisterReadResult read_registers(config::ModbusTable table, std::uint16_t start,
                                                    std::span<std::uint16_t> out) override
    {
        if (auto error = begin_read(table, start, out.size(), false)) {
            return RegisterReadResult::failure(std::move(*error));
        }
        if (auto error = fill(out)) {
            return RegisterReadResult::failure(std::move(*error));
        }
        return RegisterReadResult::success(
            RegisterBlock{.table = table, .start = start, .values = out});
    }

    [[nodiscard]] BitReadResult read_bits(config::ModbusTable table, std::uint16_t start,
                                          std::span<std::uint8_t> out) override
    {
        if (auto error = begin_read(table, start, out.size(), true)) {
            return BitReadResult::failure(std::move(*error));
        }
        std::vector<std::uint16_t> words(out.size());
        if (auto error = fill(std::span{words})) {
            return BitReadResult::failure(std::move(*error));
        }
        std::ranges::transform(words, out.begin(),
                               [](std::uint16_t word) { return word != 0U ? 1U : 0U; });
        return BitReadResult::success(BitBlock{.table = table, .start = start, .values = out});
    }

    void close() noexcept override
    {
        ++close_calls_;
        connected_ = false;
    }

    [[nodiscard]] const std::vector<Request>& requests() const noexcept { return requests_; }
    [[nodiscard]] int connect_calls() const noexcept { return connect_calls_; }
    [[nodiscard]] int close_calls() const noexcept { return close_calls_; }

private:
    std::optional<ModbusError> begin_read(config::ModbusTable table, std::uint16_t start,
                                          std::size_t quantity, bool bits)
    {
        if (auto invalid = validate_read(table, start, quantity, bits)) {
            return invalid;
        }
        if (!connected_) {
            return ModbusError{.kind = ModbusErrorKind::NotConnected,
                               .exception_code = std::nullopt,
                               .system_errno = std::nullopt,
                               .detail = "not connected"};
        }
        requests_.push_back(Request{.table = table, .start = start, .quantity = quantity});
        return std::nullopt;
    }

    // Copies the next scripted values into `out` (zero-padded), or returns the scripted error.
    std::optional<ModbusError> fill(std::span<std::uint16_t> out)
    {
        std::ranges::fill(out, std::uint16_t{0});
        if (read_script_.empty()) {
            return std::nullopt;
        }
        auto outcome = std::move(read_script_.front());
        read_script_.pop_front();
        if (auto* error = std::get_if<ModbusError>(&outcome)) {
            const bool keeps_connection = error->kind == ModbusErrorKind::ModbusException;
            connected_ = connected_ && keeps_connection;
            return std::move(*error);
        }
        const auto& values = std::get<std::vector<std::uint16_t>>(outcome);
        std::copy_n(values.begin(), std::min(values.size(), out.size()), out.begin());
        return std::nullopt;
    }

    bool connected_{false};
    int connect_calls_{0};
    int close_calls_{0};
    std::deque<std::optional<ModbusError>> connect_script_;
    std::deque<Outcome> read_script_;
    std::vector<Request> requests_;
};

} // namespace ieg::modbus::testing
