#pragma once

#include "ieg/acquisition/acquisition_source.hpp"
#include "ieg/config/gateway_config.hpp"
#include "ieg/modbus/modbus_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// Modbus TCP acquisition source: one device (one IModbusClient) polled by one owner thread.
namespace ieg::modbus {

// The item locator of a configured Modbus point: "<asset>/<tag>". Unique within a device because
// (asset, tag) is unique across the configuration, and unambiguous because neither asset nor tag
// may contain '/'.
[[nodiscard]] std::string modbus_point_locator(const config::ModbusPointConfig& point);

// Stable diagnostics carried by acquisition::Update::diagnostic for Bad updates without a value.
// Read failures use to_string(ModbusErrorKind) ("timeout", "connection_lost", "protocol_error",
// "modbus_exception", ...); decoding failures use "decode_" + to_string(DecodeError).
[[nodiscard]] std::string decode_diagnostic(DecodeError error);

// Time source and interruptible wait used by the worker. Injectable so tests can control time.
class ISourceClock
{
public:
    using time_point = std::chrono::steady_clock::time_point;

    ISourceClock() = default;
    virtual ~ISourceClock() = default;
    ISourceClock(const ISourceClock&) = delete;
    ISourceClock& operator=(const ISourceClock&) = delete;
    ISourceClock(ISourceClock&&) = delete;
    ISourceClock& operator=(ISourceClock&&) = delete;

    [[nodiscard]] virtual time_point now() = 0;
    // Blocks until `deadline` or until `stop` is requested; returns false if stop was requested.
    virtual bool wait_until(time_point deadline, const std::stop_token& stop) = 0;
};

// std::chrono::steady_clock with a condition_variable_any wait that wakes on stop requests.
class SteadySourceClock final : public ISourceClock
{
public:
    [[nodiscard]] time_point now() override;
    bool wait_until(time_point deadline, const std::stop_token& stop) override;
};

struct ModbusSourceOptions
{
    // Reconnect delay after n consecutive failures: min(initial * 2^n, max), then jitter.
    std::chrono::milliseconds reconnect_initial_delay{500};
    std::chrono::milliseconds reconnect_max_delay{30'000};
    // Each delay is multiplied by a uniformly random factor in [1 - jitter, 1] (0 = none).
    double reconnect_jitter{0.2};
    // Seed of the jitter generator; nullopt seeds from std::random_device. Tests fix it.
    std::optional<std::uint64_t> jitter_seed;
    // Upper bounds of one coalesced request (protocol maximums by default).
    std::size_t max_registers_per_read{kMaxReadRegisters};
    std::size_t max_bits_per_read{kMaxReadBits};
};

// Creates the client; called on the worker thread, which also destroys the client.
using ModbusClientFactory = std::function<std::unique_ptr<IModbusClient>()>;

// IAcquisitionSource for one Modbus TCP device.
//
// Items: each Item::locator is modbus_point_locator() of a configured point of the device; a
// subset of the points may be requested. Updates carry the source ID and the item ID, quality
// Good with the decoded value, or Bad without value; source_timestamp is always empty (Modbus
// has no sample timestamps). native_status is set only for real Modbus exception responses
// (the exception code); every other failure is described by Update::diagnostic.
//
// Threading: start() launches one worker thread that owns the client (created through the
// factory, used and destroyed only there) and invokes every callback, serialized. Requests are
// strictly sequential (no concurrent TCP requests).
//
// Polling: reads are coalesced per table into contiguous blocks (see the design document). A
// cycle reads every block once; the next cycle starts at the previous deadline plus
// poll_interval. If a cycle overruns, the missed slots are skipped (no catch-up burst) and the
// next deadline is the first future slot on the original grid.
//
// Failures: a Modbus exception marks the items of that block Bad (with the exception code) and
// keeps the connection; a multi-point block that answers with an exception is split into
// single-point reads (immediately and for later cycles), so one bad address does not hide its
// neighbours. A timeout, connection loss or protocol error marks the remaining items of the
// cycle Bad, reports Disconnected and reconnects after a bounded exponential backoff.
//
// Shutdown: stop() requests the stop and joins the worker. Waits (polling interval, backoff) end
// immediately; a request in progress cannot be interrupted, so stop() can take up to one
// request timeout (device timeout_ms), or one connect attempt (timeout_ms per resolved address,
// plus unbounded DNS resolution for host names). The worker checks the stop request between
// requests, so at most one request is in progress. Never call stop() from a callback.
class ModbusTcpSource final : public acquisition::IAcquisitionSource
{
public:
    // Uses LibModbusClient with client_options_from(device).
    explicit ModbusTcpSource(const config::ModbusDeviceConfig& device,
                             ModbusSourceOptions options = {});
    // Throws std::invalid_argument for an empty source ID, no points, a non-positive poll
    // interval, invalid options or a missing factory/clock. No thread, no network activity.
    ModbusTcpSource(config::ModbusDeviceConfig device, ModbusClientFactory factory,
                    ModbusSourceOptions options = {},
                    std::shared_ptr<ISourceClock> clock = std::make_shared<SteadySourceClock>());
    ~ModbusTcpSource() override;

    ModbusTcpSource(const ModbusTcpSource&) = delete;
    ModbusTcpSource& operator=(const ModbusTcpSource&) = delete;
    ModbusTcpSource(ModbusTcpSource&&) = delete;
    ModbusTcpSource& operator=(ModbusTcpSource&&) = delete;

    [[nodiscard]] const std::string& id() const noexcept override;
    void start(std::vector<acquisition::Item> items, acquisition::Callbacks callbacks) override;
    void stop() noexcept override;

private:
    class Worker;

    enum class Phase : std::uint8_t
    {
        Idle,
        Started,
        Stopped
    };

    const config::ModbusDeviceConfig device_;
    const ModbusClientFactory factory_;
    const ModbusSourceOptions options_;
    const std::shared_ptr<ISourceClock> clock_;

    std::mutex lifecycle_mutex_;
    Phase phase_{Phase::Idle};
    std::unique_ptr<Worker> worker_;
    std::jthread thread_;
};

} // namespace ieg::modbus
