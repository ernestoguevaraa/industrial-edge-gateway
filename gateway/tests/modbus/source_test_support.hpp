#pragma once

// Test doubles for ModbusTcpSource: a manually stepped clock, a thread-safe fake Modbus device
// whose clients run on the source's worker thread, and a thread-safe callback recorder.

#include "ieg/acquisition/acquisition_source.hpp"
#include "ieg/modbus/modbus_client.hpp"
#include "ieg/modbus/modbus_tcp_source.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ieg::modbus::testing {

// Virtual time: wait_until() blocks until the test advances the clock past the deadline (or
// stop is requested). Every deadline the worker waits for is recorded.
class ManualClock final : public ISourceClock
{
public:
    [[nodiscard]] time_point now() override
    {
        const std::lock_guard lock(mutex_);
        return now_;
    }

    bool wait_until(time_point deadline, const std::stop_token& stop) override
    {
        // Registered before taking the lock: it may run immediately on this thread.
        const std::stop_callback wake(stop, [this] {
            const std::lock_guard lock(mutex_);
            changed_.notify_all();
        });
        std::unique_lock lock(mutex_);
        deadlines_.push_back(deadline);
        ++waiting_;
        changed_.notify_all();
        changed_.wait(lock, [&] { return now_ >= deadline || stop.stop_requested(); });
        --waiting_;
        return !stop.stop_requested();
    }

    void advance(std::chrono::nanoseconds step)
    {
        {
            const std::lock_guard lock(mutex_);
            now_ += step;
        }
        changed_.notify_all();
    }

    // Advances to the latest recorded deadline (releases a waiting worker).
    void advance_to_deadline()
    {
        {
            const std::lock_guard lock(mutex_);
            if (!deadlines_.empty()) {
                now_ = std::max(now_, deadlines_.back());
            }
        }
        changed_.notify_all();
    }

    // Waits (in real time) until the worker has recorded `count` waits in total and is blocked.
    bool wait_for_waits(std::size_t count,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout,
                                 [&] { return deadlines_.size() >= count && waiting_ > 0; });
    }

    [[nodiscard]] std::vector<time_point> deadlines() const
    {
        const std::lock_guard lock(mutex_);
        return deadlines_;
    }

    // The most recent recorded deadline (start() if the worker has not waited yet).
    [[nodiscard]] time_point last_deadline() const
    {
        const std::lock_guard lock(mutex_);
        return deadlines_.empty() ? start() : deadlines_.back();
    }

    // The initial virtual time.
    [[nodiscard]] static time_point start() noexcept { return time_point{}; }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    time_point now_; // starts at the zero time point, see start()
    std::vector<time_point> deadlines_;
    int waiting_{0};
};

// Thread-safe fake Modbus device. make_client() returns clients bound to it; all of their calls
// are recorded together with the calling thread.
class FakeModbusDevice
{
public:
    struct Request
    {
        config::ModbusTable table{};
        std::uint16_t start{};
        std::size_t count{};

        friend bool operator==(const Request&, const Request&) = default;
    };

    void set_register(config::ModbusTable table, std::uint16_t address, std::uint16_t value)
    {
        const std::lock_guard lock(mutex_);
        registers_[{table, address}] = value;
    }

    void set_bit(config::ModbusTable table, std::uint16_t address, bool value)
    {
        set_register(table, address, value ? 1U : 0U);
    }

    // Any read that touches [first, last] of `table` answers with this exception code.
    void set_exception(config::ModbusTable table, std::uint16_t first, std::uint16_t last,
                       std::uint8_t code)
    {
        const std::lock_guard lock(mutex_);
        exceptions_.push_back(ExceptionRange{table, first, last, code});
    }

    // The next `count` connect() calls fail with ConnectionFailed.
    void fail_next_connects(int count)
    {
        const std::lock_guard lock(mutex_);
        failing_connects_ = count;
    }

    // The next read fails with `error` (closing the connection unless it is an exception).
    void fail_next_read(ModbusError error)
    {
        const std::lock_guard lock(mutex_);
        read_failures_.push_back(std::move(error));
    }

    // Called inside every read (on the worker thread), e.g. to advance a ManualClock.
    void on_read(std::function<void()> hook)
    {
        const std::lock_guard lock(mutex_);
        read_hook_ = std::move(hook);
    }

    void throw_on_next_client(int count)
    {
        const std::lock_guard lock(mutex_);
        throwing_factories_ = count;
    }

    [[nodiscard]] std::unique_ptr<IModbusClient> make_client();

    [[nodiscard]] std::vector<Request> requests() const
    {
        const std::lock_guard lock(mutex_);
        return requests_;
    }
    [[nodiscard]] int connects() const
    {
        const std::lock_guard lock(mutex_);
        return connects_;
    }
    [[nodiscard]] int clients_alive() const
    {
        const std::lock_guard lock(mutex_);
        return clients_alive_;
    }
    [[nodiscard]] std::set<std::thread::id> client_threads() const
    {
        const std::lock_guard lock(mutex_);
        return client_threads_;
    }
    [[nodiscard]] int max_concurrent_requests() const
    {
        const std::lock_guard lock(mutex_);
        return max_in_flight_;
    }

private:
    friend class FakeDeviceClient;

    struct ExceptionRange
    {
        config::ModbusTable table;
        std::uint16_t first;
        std::uint16_t last;
        std::uint8_t code;
    };

    void touch_locked() { client_threads_.insert(std::this_thread::get_id()); }

    static ModbusError make_error(ModbusErrorKind kind, std::optional<std::uint8_t> code,
                                  std::string detail)
    {
        return ModbusError{.kind = kind,
                           .exception_code = code,
                           .system_errno = std::nullopt,
                           .detail = std::move(detail)};
    }

    // Serves one read into `out` (one element per address); nullopt on success.
    std::optional<ModbusError> serve(config::ModbusTable table, std::uint16_t start,
                                     std::size_t count,
                                     const std::function<void(std::size_t, std::uint16_t)>& out)
    {
        std::function<void()> hook;
        {
            const std::lock_guard lock(mutex_);
            touch_locked();
            hook = read_hook_;
            max_in_flight_ = std::max(max_in_flight_, ++in_flight_);
        }
        if (hook) {
            hook();
        }
        const std::lock_guard lock(mutex_);
        --in_flight_;
        requests_.push_back(Request{.table = table, .start = start, .count = count});
        if (!read_failures_.empty()) {
            auto error = std::move(read_failures_.front());
            read_failures_.pop_front();
            return error;
        }
        for (const auto& range : exceptions_) {
            const std::size_t end = static_cast<std::size_t>(start) + count - 1;
            if (range.table == table && range.first <= end && start <= range.last) {
                return make_error(ModbusErrorKind::ModbusException, range.code, "exception");
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto found = registers_.find({table, static_cast<std::uint16_t>(start + i)});
            out(i, found == registers_.end() ? std::uint16_t{0} : found->second);
        }
        return std::nullopt;
    }

    mutable std::mutex mutex_;
    std::map<std::pair<config::ModbusTable, std::uint16_t>, std::uint16_t> registers_;
    std::vector<ExceptionRange> exceptions_;
    std::deque<ModbusError> read_failures_;
    std::function<void()> read_hook_;
    std::vector<Request> requests_;
    std::set<std::thread::id> client_threads_;
    int failing_connects_{0};
    int throwing_factories_{0};
    int connects_{0};
    int clients_alive_{0};
    int in_flight_{0};
    int max_in_flight_{0};
};

class FakeDeviceClient final : public IModbusClient
{
public:
    explicit FakeDeviceClient(FakeModbusDevice& device) : device_(&device)
    {
        const std::lock_guard lock(device_->mutex_);
        device_->touch_locked();
        ++device_->clients_alive_;
    }

    ~FakeDeviceClient() override
    {
        const std::lock_guard lock(device_->mutex_);
        device_->touch_locked();
        --device_->clients_alive_;
    }

    FakeDeviceClient(const FakeDeviceClient&) = delete;
    FakeDeviceClient& operator=(const FakeDeviceClient&) = delete;
    FakeDeviceClient(FakeDeviceClient&&) = delete;
    FakeDeviceClient& operator=(FakeDeviceClient&&) = delete;

    [[nodiscard]] ConnectResult connect() override
    {
        const std::lock_guard lock(device_->mutex_);
        device_->touch_locked();
        ++device_->connects_;
        connected_ = device_->failing_connects_ == 0;
        if (!connected_) {
            --device_->failing_connects_;
            return ConnectResult::failure(FakeModbusDevice::make_error(
                ModbusErrorKind::ConnectionFailed, std::nullopt, "refused"));
        }
        return ConnectResult::success(std::monostate{});
    }

    [[nodiscard]] bool connected() const noexcept override { return connected_; }

    [[nodiscard]] RegisterReadResult read_registers(config::ModbusTable table, std::uint16_t start,
                                                    std::span<std::uint16_t> out) override
    {
        if (auto error = precheck(table, start, out.size(), false)) {
            return RegisterReadResult::failure(std::move(*error));
        }
        if (auto error = device_->serve(table, start, out.size(),
                                        [&](std::size_t i, std::uint16_t v) { out[i] = v; })) {
            return RegisterReadResult::failure(after_failure(std::move(*error)));
        }
        return RegisterReadResult::success(
            RegisterBlock{.table = table, .start = start, .values = out});
    }

    [[nodiscard]] BitReadResult read_bits(config::ModbusTable table, std::uint16_t start,
                                          std::span<std::uint8_t> out) override
    {
        if (auto error = precheck(table, start, out.size(), true)) {
            return BitReadResult::failure(std::move(*error));
        }
        if (auto error =
                device_->serve(table, start, out.size(), [&](std::size_t i, std::uint16_t v) {
                    out[i] = v != 0U ? 1U : 0U;
                })) {
            return BitReadResult::failure(after_failure(std::move(*error)));
        }
        return BitReadResult::success(BitBlock{.table = table, .start = start, .values = out});
    }

    void close() noexcept override
    {
        const std::lock_guard lock(device_->mutex_);
        device_->touch_locked();
        connected_ = false;
    }

private:
    [[nodiscard]] std::optional<ModbusError>
    precheck(config::ModbusTable table, std::uint16_t start, std::size_t count, bool bits) const
    {
        if (auto invalid = validate_read(table, start, count, bits)) {
            return invalid;
        }
        if (!connected_) {
            return FakeModbusDevice::make_error(ModbusErrorKind::NotConnected, std::nullopt,
                                                "not connected");
        }
        return std::nullopt;
    }

    ModbusError after_failure(ModbusError error)
    {
        connected_ = connected_ && error.kind == ModbusErrorKind::ModbusException;
        return error;
    }

    FakeModbusDevice* device_;
    bool connected_{false};
};

inline std::unique_ptr<IModbusClient> FakeModbusDevice::make_client()
{
    {
        const std::lock_guard lock(mutex_);
        if (throwing_factories_ > 0) {
            --throwing_factories_;
            throw std::runtime_error("cannot create client");
        }
    }
    return std::make_unique<FakeDeviceClient>(*this);
}

// Thread-safe sink for acquisition callbacks.
class SourceRecorder
{
public:
    acquisition::Callbacks callbacks()
    {
        return acquisition::Callbacks{
            .on_update = [this](const acquisition::Update& u) { record(updates_, u); },
            .on_status = [this](const acquisition::Status& s) { record(statuses_, s); }};
    }

    template <typename Predicate>
    bool wait_until(Predicate predicate,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(updates_, statuses_); });
    }

    bool wait_for_updates(std::size_t count)
    {
        return wait_until(
            [count](const auto& updates, const auto&) { return updates.size() >= count; });
    }

    bool wait_for_state(acquisition::ConnectionState state, std::size_t occurrences = 1)
    {
        return wait_until([&](const auto&, const auto& statuses) {
            return static_cast<std::size_t>(std::ranges::count_if(
                       statuses, [&](const acquisition::Status& s) { return s.state == state; })) >=
                   occurrences;
        });
    }

    [[nodiscard]] std::vector<acquisition::Update> updates() const
    {
        const std::lock_guard lock(mutex_);
        return updates_;
    }

    [[nodiscard]] std::vector<acquisition::Status> statuses() const
    {
        const std::lock_guard lock(mutex_);
        return statuses_;
    }

    // The most recent status (a default Status if none was reported).
    [[nodiscard]] acquisition::Status last_status() const
    {
        const std::lock_guard lock(mutex_);
        return statuses_.empty() ? acquisition::Status{} : statuses_.back();
    }

    [[nodiscard]] std::size_t total() const
    {
        const std::lock_guard lock(mutex_);
        return updates_.size() + statuses_.size();
    }

private:
    template <typename T>
    void record(std::vector<T>& into, const T& value)
    {
        {
            const std::lock_guard lock(mutex_);
            into.push_back(value);
        }
        changed_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<acquisition::Update> updates_;
    std::vector<acquisition::Status> statuses_;
};

} // namespace ieg::modbus::testing
