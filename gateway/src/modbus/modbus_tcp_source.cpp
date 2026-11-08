#include "ieg/modbus/modbus_tcp_source.hpp"

#include "common/backoff.hpp"
#include "ieg/modbus/decoder.hpp"
#include "ieg/modbus/libmodbus_client.hpp"
#include "modbus/poll_plan.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <format>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ieg::modbus {
namespace {

using acquisition::ConnectionState;
using acquisition::Update;
using detail::PlannedPoint;
using detail::ReadBlock;

void validate_construction(const config::ModbusDeviceConfig& device,
                           const ModbusSourceOptions& options)
{
    std::string errors;
    if (device.source_id.empty()) {
        errors += "\n - source_id must not be empty";
    }
    if (device.points.empty()) {
        errors += "\n - the device needs at least one point";
    }
    if (device.poll_interval <= std::chrono::milliseconds::zero()) {
        errors += "\n - poll_interval must be positive";
    }
    if (options.reconnect_initial_delay <= std::chrono::milliseconds::zero() ||
        options.reconnect_max_delay < options.reconnect_initial_delay) {
        errors += "\n - reconnect delays must satisfy 0 < initial <= max";
    }
    if (!std::isfinite(options.reconnect_jitter) || options.reconnect_jitter < 0.0 ||
        options.reconnect_jitter >= 1.0) {
        errors += "\n - reconnect_jitter must be in [0, 1)";
    }
    if (options.max_registers_per_read == 0 || options.max_registers_per_read > kMaxReadRegisters ||
        options.max_bits_per_read == 0 || options.max_bits_per_read > kMaxReadBits) {
        errors +=
            "\n - per-read limits must be within the Modbus maximums (125 registers, 2000 bits)";
    }
    if (!errors.empty()) {
        throw std::invalid_argument("invalid ModbusTcpSource configuration:" + errors);
    }
}

std::string describe(const ModbusError& error)
{
    return std::format("{}: {}", to_string(error.kind), error.detail);
}

} // namespace

std::string modbus_point_locator(const config::ModbusPointConfig& point)
{
    return point.asset + "/" + point.tag;
}

std::string decode_diagnostic(DecodeError error)
{
    return std::format("decode_{}", to_string(error));
}

ISourceClock::time_point SteadySourceClock::now()
{
    return std::chrono::steady_clock::now();
}

bool SteadySourceClock::wait_until(time_point deadline, const std::stop_token& stop)
{
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    // Returns early (and only) when stop is requested; the predicate never becomes true.
    static_cast<void>(wake.wait_until(lock, stop, deadline, [] { return false; }));
    return !stop.stop_requested();
}

// ---- Worker: everything that runs on the owner thread --------------------------------------

class ModbusTcpSource::Worker
{
public:
    Worker(const config::ModbusDeviceConfig& device, ModbusClientFactory factory,
           const ModbusSourceOptions& options, std::shared_ptr<ISourceClock> clock,
           std::vector<PlannedPoint> points, acquisition::Callbacks callbacks)
        : source_id_(device.source_id), poll_interval_(device.poll_interval),
          factory_(std::move(factory)), options_(options), clock_(std::move(clock)),
          points_(std::move(points)),
          plan_(detail::plan_reads(points_, {.max_registers = options.max_registers_per_read,
                                             .max_bits = options.max_bits_per_read})),
          callbacks_(std::move(callbacks)),
          rng_(options.jitter_seed.value_or(std::random_device{}()))
    {}

    [[nodiscard]] std::thread::id thread_id() const noexcept { return thread_id_.load(); }

    void run(const std::stop_token& stop) noexcept
    {
        try {
            thread_id_ = std::this_thread::get_id();
            run_loop(stop);
        } catch (...) {
            // Only a throwing callback (a contract violation) or allocation failure gets here.
            std::terminate();
        }
    }

private:
    enum class CycleOutcome : std::uint8_t
    {
        Completed,
        ConnectionClosed,
        Stopped
    };

    void run_loop(const std::stop_token& stop)
    {
        std::unique_ptr<IModbusClient> client;
        unsigned failures = 0;
        while (!stop.stop_requested()) {
            if (!client && !create_client(client)) {
                if (!wait_backoff(failures++, stop)) {
                    break;
                }
                continue;
            }
            report(ConnectionState::Connecting, "");
            const auto connected = client->connect();
            if (!connected.ok()) {
                report(ConnectionState::Disconnected, describe(connected.error()));
                if (!wait_backoff(failures++, stop)) {
                    break;
                }
                continue;
            }
            failures = 0;
            report(ConnectionState::Connected, "");
            if (poll_while_connected(*client, stop) == CycleOutcome::Stopped) {
                break;
            }
            // Connection closed after a failure (already reported): back off, then reconnect.
            if (!wait_backoff(failures++, stop)) {
                break;
            }
        }
        if (client) {
            client->close(); // deliberate stop: no Disconnected status
        }
    }

    bool create_client(std::unique_ptr<IModbusClient>& client)
    {
        std::string problem;
        try {
            client = factory_();
            if (client) {
                return true;
            }
            problem = "the client factory returned no client";
        } catch (const std::exception& error) {
            problem = error.what();
        }
        report(ConnectionState::Disconnected, "client_error: " + problem);
        return false;
    }

    CycleOutcome poll_while_connected(IModbusClient& client, const std::stop_token& stop)
    {
        auto deadline = clock_->now();
        while (true) {
            const auto outcome = run_cycle(client, stop);
            if (outcome != CycleOutcome::Completed) {
                return outcome;
            }
            deadline = detail::next_poll_deadline(deadline, clock_->now(), poll_interval_);
            if (!clock_->wait_until(deadline, stop)) {
                return CycleOutcome::Stopped;
            }
        }
    }

    // Reads every block once. Blocks that answer with a Modbus exception are split into
    // single-point blocks (in place, so later cycles keep the split).
    CycleOutcome run_cycle(IModbusClient& client, const std::stop_token& stop)
    {
        std::size_t index = 0;
        while (index < plan_.size()) {
            if (stop.stop_requested()) {
                return CycleOutcome::Stopped;
            }
            const auto result = read_block(client, plan_[index]);
            if (!result) {
                ++index;
                continue;
            }
            const ModbusError& error = *result;
            if (error.kind == ModbusErrorKind::ModbusException && plan_[index].points.size() > 1) {
                auto singles = detail::split_block(plan_[index], points_);
                plan_.erase(plan_.begin() + static_cast<std::ptrdiff_t>(index));
                plan_.insert(plan_.begin() + static_cast<std::ptrdiff_t>(index),
                             std::make_move_iterator(singles.begin()),
                             std::make_move_iterator(singles.end()));
                continue; // re-read the same addresses point by point
            }
            emit_failure(plan_[index], error);
            if (!client.connected()) {
                // The client closed the connection: the rest of this cycle cannot be read.
                for (std::size_t rest = index + 1; rest < plan_.size(); ++rest) {
                    emit_failure(plan_[rest], error);
                }
                report(ConnectionState::Disconnected, describe(error));
                return CycleOutcome::ConnectionClosed;
            }
            ++index;
        }
        return CycleOutcome::Completed;
    }

    // Reads one block and emits its updates on success; returns the read error otherwise.
    std::optional<ModbusError> read_block(IModbusClient& client, const ReadBlock& block)
    {
        if (config::is_bit_table(block.table)) {
            bits_.assign(block.count, 0);
            const auto result = client.read_bits(block.table, block.start, bits_);
            if (!result.ok()) {
                return result.error();
            }
            emit_decoded(block, result.value());
            return std::nullopt;
        }
        registers_.assign(block.count, 0);
        const auto result = client.read_registers(block.table, block.start, registers_);
        if (!result.ok()) {
            return result.error();
        }
        emit_decoded(block, result.value());
        return std::nullopt;
    }

    template <typename Block>
    void emit_decoded(const ReadBlock& block, const Block& data)
    {
        for (const std::size_t index : block.points) {
            const auto& planned = points_[index];
            const auto decoded = decode(planned.point, data);
            if (decoded.ok()) {
                emit(planned.item, decoded.value(), domain::DataQuality::Good, std::nullopt, "");
            } else {
                emit(planned.item, std::nullopt, domain::DataQuality::Bad, std::nullopt,
                     decode_diagnostic(decoded.error()));
            }
        }
    }

    void emit_failure(const ReadBlock& block, const ModbusError& error)
    {
        // Only a real Modbus exception response has a native status: its exception code.
        const auto native =
            error.exception_code
                ? std::optional<acquisition::NativeStatusCode>{*error.exception_code}
                : std::nullopt;
        const std::string diagnostic{to_string(error.kind)};
        for (const std::size_t index : block.points) {
            emit(points_[index].item, std::nullopt, domain::DataQuality::Bad, native, diagnostic);
        }
    }

    void emit(acquisition::ItemId item, std::optional<domain::TelemetryValue> value,
              domain::DataQuality quality, std::optional<acquisition::NativeStatusCode> native,
              std::string diagnostic)
    {
        callbacks_.on_update(Update{.source_id = source_id_,
                                    .item_id = item,
                                    .value = std::move(value),
                                    .quality = quality,
                                    .source_timestamp = std::nullopt, // Modbus has none
                                    .native_status = native,
                                    .diagnostic = std::move(diagnostic)});
    }

    void report(ConnectionState state, std::string detail)
    {
        callbacks_.on_status(acquisition::Status{
            .source_id = source_id_, .state = state, .detail = std::move(detail)});
    }

    bool wait_backoff(unsigned failures, const std::stop_token& stop)
    {
        const auto base = ieg::detail::backoff_delay(failures, options_.reconnect_initial_delay,
                                                     options_.reconnect_max_delay);
        double factor = 1.0;
        if (options_.reconnect_jitter > 0.0) {
            std::uniform_real_distribution<double> jitter(1.0 - options_.reconnect_jitter, 1.0);
            factor = jitter(rng_);
        }
        const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double, std::milli>(static_cast<double>(base.count()) * factor));
        return clock_->wait_until(clock_->now() + std::max(delay, std::chrono::milliseconds{1}),
                                  stop);
    }

    const std::string source_id_;
    const std::chrono::milliseconds poll_interval_;
    const ModbusClientFactory factory_;
    const ModbusSourceOptions options_;
    const std::shared_ptr<ISourceClock> clock_;
    const std::vector<PlannedPoint> points_;
    std::vector<ReadBlock> plan_;
    const acquisition::Callbacks callbacks_;
    std::mt19937_64 rng_;
    std::vector<std::uint16_t> registers_;
    std::vector<std::uint8_t> bits_;
    std::atomic<std::thread::id> thread_id_; // value-initialized (no thread)
};

// ---- ModbusTcpSource -----------------------------------------------------------------------

ModbusTcpSource::ModbusTcpSource(const config::ModbusDeviceConfig& device,
                                 ModbusSourceOptions options)
    : ModbusTcpSource(
          device,
          [client_options = client_options_from(device)]() -> std::unique_ptr<IModbusClient> {
              return std::make_unique<LibModbusClient>(client_options);
          },
          options)
{}

ModbusTcpSource::ModbusTcpSource(config::ModbusDeviceConfig device, ModbusClientFactory factory,
                                 ModbusSourceOptions options, std::shared_ptr<ISourceClock> clock)
    : device_(std::move(device)), factory_(std::move(factory)), options_(options),
      clock_(std::move(clock))
{
    validate_construction(device_, options_);
    if (!factory_ || !clock_) {
        throw std::invalid_argument("ModbusTcpSource needs a client factory and a clock");
    }
}

ModbusTcpSource::~ModbusTcpSource()
{
    stop();
}

const std::string& ModbusTcpSource::id() const noexcept
{
    return device_.source_id;
}

void ModbusTcpSource::start(std::vector<acquisition::Item> items, acquisition::Callbacks callbacks)
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (phase_ != Phase::Idle) {
        throw std::logic_error("ModbusTcpSource::start() called after start() or stop()");
    }
    if (!callbacks.on_update || !callbacks.on_status) {
        throw std::invalid_argument("ModbusTcpSource requires both callbacks");
    }
    if (items.empty()) {
        throw std::invalid_argument("ModbusTcpSource requires at least one item");
    }
    std::map<std::string, std::size_t> by_locator;
    for (std::size_t index = 0; index < device_.points.size(); ++index) {
        by_locator.emplace(modbus_point_locator(device_.points[index]), index);
    }
    std::unordered_set<acquisition::ItemId> ids;
    std::unordered_set<std::size_t> used_points;
    std::vector<PlannedPoint> planned;
    planned.reserve(items.size());
    for (const auto& item : items) {
        const auto found = by_locator.find(item.locator);
        if (found == by_locator.end()) {
            throw std::invalid_argument(
                std::format("ModbusTcpSource '{}' has no point with locator '{}'",
                            device_.source_id, item.locator));
        }
        if (!ids.insert(item.id).second || !used_points.insert(found->second).second) {
            throw std::invalid_argument("ModbusTcpSource received a duplicate item ID or locator");
        }
        planned.push_back(PlannedPoint{.item = item.id, .point = device_.points[found->second]});
    }
    // Published before the thread starts, so a callback thread observes it (thread start
    // synchronizes with everything written before).
    worker_ = std::make_unique<Worker>(device_, factory_, options_, clock_, std::move(planned),
                                       std::move(callbacks));
    try {
        thread_ =
            std::jthread([raw = worker_.get()](const std::stop_token& stop) { raw->run(stop); });
    } catch (...) {
        phase_ = Phase::Stopped; // nothing runs; the single start is consumed
        throw;
    }
    phase_ = Phase::Started;
}

void ModbusTcpSource::stop() noexcept
{
    try {
        if (worker_ && std::this_thread::get_id() == worker_->thread_id()) {
            // Precondition violation (called from a callback): joining would self-deadlock.
            thread_.request_stop();
            return;
        }
        const std::lock_guard lock(lifecycle_mutex_);
        phase_ = Phase::Stopped;
        if (thread_.joinable()) {
            thread_.request_stop(); // ends waits at once; a request in progress completes first
            thread_.join();         // after this no callback can run
        }
    } catch (...) {
        std::terminate(); // only reachable if locking or joining fails
    }
}

} // namespace ieg::modbus
