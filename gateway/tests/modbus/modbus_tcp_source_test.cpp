// ModbusTcpSource with a thread-safe fake device and a manually stepped clock: deterministic,
// no network and no real sleeps for the scheduling and backoff behaviour.

#include "ieg/modbus/modbus_tcp_source.hpp"
#include "modbus/source_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace ieg::modbus {
namespace {

using namespace std::chrono_literals;
using acquisition::ConnectionState;
using acquisition::Item;
using acquisition::Update;
using config::ModbusDataType;
using config::ModbusTable;
using config::ModbusWordOrder;
using domain::DataQuality;
using testing::FakeModbusDevice;
using testing::ManualClock;
using testing::SourceRecorder;
using Request = FakeModbusDevice::Request;

constexpr auto kInterval = 1000ms;
constexpr const char* kSource = "compressor-plc";
constexpr const char* kAsset = "site01.line01.comp01";

config::ModbusPointConfig point(const std::string& tag, ModbusTable table, std::uint16_t offset,
                                ModbusDataType type, std::optional<ModbusWordOrder> order = {},
                                std::optional<double> scale = {})
{
    config::ModbusPointConfig result;
    result.asset = kAsset;
    result.tag = tag;
    result.table = table;
    result.offset = offset;
    result.type = type;
    result.word_order = order;
    result.scale = scale;
    return result;
}

// pressure 0, temperature 1, current 2..3 (one block) | spare 10 (separate block) | running coil
config::ModbusDeviceConfig device_config()
{
    config::ModbusDeviceConfig device;
    device.source_id = kSource;
    device.host = "fake";
    device.unit_id = 1;
    device.poll_interval = kInterval;
    device.timeout = 500ms;
    device.points = {
        point("pressure", ModbusTable::HoldingRegisters, 0, ModbusDataType::UInt16, {}, 0.1),
        point("temperature", ModbusTable::HoldingRegisters, 1, ModbusDataType::Int16),
        point("current", ModbusTable::HoldingRegisters, 2, ModbusDataType::Float32,
              ModbusWordOrder::LowWordFirst),
        point("spare", ModbusTable::HoldingRegisters, 10, ModbusDataType::UInt16),
        point("running", ModbusTable::Coils, 0, ModbusDataType::Bool),
    };
    return device;
}

std::vector<Item> all_items()
{
    std::vector<Item> items;
    const auto device = device_config();
    for (std::size_t i = 0; i < device.points.size(); ++i) {
        items.push_back(Item{.id = static_cast<acquisition::ItemId>(100 + i),
                             .locator = modbus_point_locator(device.points[i])});
    }
    return items;
}

ModbusSourceOptions deterministic_options()
{
    return ModbusSourceOptions{.reconnect_initial_delay = 100ms,
                               .reconnect_max_delay = 400ms,
                               .reconnect_jitter = 0.0,
                               .jitter_seed = 1,
                               .max_registers_per_read = kMaxReadRegisters,
                               .max_bits_per_read = kMaxReadBits};
}

ModbusError error_of(ModbusErrorKind kind, std::optional<std::uint8_t> code = std::nullopt)
{
    return ModbusError{
        .kind = kind, .exception_code = code, .system_errno = std::nullopt, .detail = "injected"};
}

// The most recent update for `item`.
std::optional<Update> last_for(const std::vector<Update>& updates, acquisition::ItemId item)
{
    const auto found = std::ranges::find_if(updates.rbegin(), updates.rend(),
                                            [&](const Update& u) { return u.item_id == item; });
    return found == updates.rend() ? std::nullopt : std::optional{*found};
}

Update good(acquisition::ItemId item, domain::TelemetryValue value)
{
    return Update{.source_id = kSource,
                  .item_id = item,
                  .value = std::move(value),
                  .quality = DataQuality::Good,
                  .source_timestamp = std::nullopt,
                  .native_status = std::nullopt,
                  .diagnostic = ""};
}

Update bad(acquisition::ItemId item, std::string diagnostic,
           std::optional<acquisition::NativeStatusCode> native = std::nullopt)
{
    return Update{.source_id = kSource,
                  .item_id = item,
                  .value = std::nullopt,
                  .quality = DataQuality::Bad,
                  .source_timestamp = std::nullopt,
                  .native_status = native,
                  .diagnostic = std::move(diagnostic)};
}

class SourceHarness
{
public:
    explicit SourceHarness(ModbusSourceOptions options = deterministic_options(),
                           config::ModbusDeviceConfig device = device_config())
        : clock_(std::make_shared<ManualClock>()),
          source_(std::make_unique<ModbusTcpSource>(
              std::move(device), [this] { return device_.make_client(); }, options, clock_))
    {
        device_.set_register(ModbusTable::HoldingRegisters, 0, 75);     // 7.5 with scale 0.1
        device_.set_register(ModbusTable::HoldingRegisters, 1, 0xFF9C); // -100
        device_.set_register(ModbusTable::HoldingRegisters, 2, 0x0FDB); // float pi, "CDAB"
        device_.set_register(ModbusTable::HoldingRegisters, 3, 0x4049);
        device_.set_register(ModbusTable::HoldingRegisters, 10, 42);
        device_.set_bit(ModbusTable::Coils, 0, true);
    }

    void start(std::vector<Item> items = all_items())
    {
        source_->start(std::move(items), recorder_.callbacks());
    }

    // Waits until the worker has completed `cycles` polling cycles and waits for the next one.
    void wait_for_cycle_waits(std::size_t waits) { ASSERT_TRUE(clock_->wait_for_waits(waits)); }

    FakeModbusDevice& device() { return device_; }
    ManualClock& clock() { return *clock_; }
    SourceRecorder& recorder() { return recorder_; }
    ModbusTcpSource& source() { return *source_; }
    void destroy() { source_.reset(); }

private:
    FakeModbusDevice device_; // outlives the source (declared first)
    SourceRecorder recorder_;
    std::shared_ptr<ManualClock> clock_;
    std::unique_ptr<ModbusTcpSource> source_;
};

// float32 pi (registers 0x4049 0x0FDB) widened exactly to double.
constexpr double kFloatPi = static_cast<double>(std::numbers::pi_v<float>);

bool construction_fails(const config::ModbusDeviceConfig& device, ModbusClientFactory factory,
                        const ModbusSourceOptions& options = deterministic_options(),
                        std::shared_ptr<ISourceClock> clock = std::make_shared<ManualClock>())
{
    try {
        const ModbusTcpSource source{device, std::move(factory), options, std::move(clock)};
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

// Every item's latest update is Bad with `diagnostic`.
::testing::AssertionResult all_bad(const std::vector<Update>& updates,
                                   const std::vector<acquisition::ItemId>& items,
                                   const std::string& diagnostic)
{
    for (const auto item : items) {
        if (last_for(updates, item) != bad(item, diagnostic)) {
            return ::testing::AssertionFailure() << "item " << item << " is not Bad/" << diagnostic;
        }
    }
    return ::testing::AssertionSuccess();
}

// Lets the worker reach `waits` recorded waits, releasing each wait before the last one.
void release_waits(ManualClock& clock, std::size_t waits)
{
    for (std::size_t wait = 1; wait <= waits; ++wait) {
        ASSERT_TRUE(clock.wait_for_waits(wait));
        if (wait < waits) {
            clock.advance_to_deadline();
        }
    }
}

// The first `count` reconnect delays of a source whose connects always fail.
std::vector<std::chrono::steady_clock::duration> reconnect_delays(std::uint64_t seed, double jitter,
                                                                  std::size_t count)
{
    auto options = deterministic_options();
    options.reconnect_jitter = jitter;
    options.jitter_seed = seed;
    SourceHarness harness{options};
    harness.device().fail_next_connects(1000);
    harness.start();
    std::vector<std::chrono::steady_clock::duration> delays;
    auto previous = ManualClock::start();
    for (std::size_t wait = 1; wait <= count; ++wait) {
        if (!harness.clock().wait_for_waits(wait)) {
            return {};
        }
        const auto deadline = harness.clock().last_deadline();
        delays.push_back(deadline - previous);
        previous = deadline;
        harness.clock().advance_to_deadline();
    }
    return delays;
}

// Each delay lies in [base * (1 - jitter), base] (1 ms tolerance for truncation).
::testing::AssertionResult
within_jitter(const std::vector<std::chrono::steady_clock::duration>& delays,
              const std::vector<std::chrono::milliseconds>& bases, double jitter)
{
    if (delays.size() != bases.size()) {
        return ::testing::AssertionFailure() << "expected " << bases.size() << " delays";
    }
    for (std::size_t i = 0; i < delays.size(); ++i) {
        const auto low =
            std::chrono::duration_cast<std::chrono::milliseconds>(bases[i] * (1.0 - jitter)) -
            std::chrono::milliseconds{1};
        if (delays[i] < low || delays[i] > bases[i]) {
            return ::testing::AssertionFailure() << "delay " << i << " outside the jitter range";
        }
    }
    return ::testing::AssertionSuccess();
}

// ---- Construction and start validation -------------------------------------------------------

TEST(ModbusTcpSource, ExposesTheSourceIdAndLocators)
{
    SourceHarness harness;
    EXPECT_EQ(std::string{harness.source().id()}, kSource);
    EXPECT_EQ(modbus_point_locator(device_config().points[0]), "site01.line01.comp01/pressure");
}

const ModbusClientFactory kNullFactory = [] { return std::unique_ptr<IModbusClient>{}; };

TEST(ModbusTcpSource, RejectsAnUnusableDeviceConfiguration)
{
    const auto& factory = kNullFactory;
    EXPECT_FALSE(construction_fails(device_config(), factory));
    auto no_id = device_config();
    no_id.source_id.clear();
    EXPECT_TRUE(construction_fails(no_id, factory));
    auto no_points = device_config();
    no_points.points.clear();
    EXPECT_TRUE(construction_fails(no_points, factory));
    auto no_interval = device_config();
    no_interval.poll_interval = 0ms;
    EXPECT_TRUE(construction_fails(no_interval, factory));
}

TEST(ModbusTcpSource, RejectsInvalidOptionsAndMissingCollaborators)
{
    const auto& factory = kNullFactory;
    EXPECT_TRUE(construction_fails(device_config(), ModbusClientFactory{}));
    EXPECT_TRUE(construction_fails(device_config(), factory, deterministic_options(), nullptr));
    for (const double jitter : {-0.1, 1.0, std::numeric_limits<double>::quiet_NaN()}) {
        auto options = deterministic_options();
        options.reconnect_jitter = jitter;
        EXPECT_TRUE(construction_fails(device_config(), factory, options)) << jitter;
    }
    auto bad_limit = deterministic_options();
    bad_limit.max_registers_per_read = 126;
    EXPECT_TRUE(construction_fails(device_config(), factory, bad_limit));
}

TEST(ModbusTcpSource, RejectsInvalidItemsWithoutConsumingTheStart)
{
    SourceHarness harness;
    auto& source = harness.source();
    SourceRecorder recorder;
    EXPECT_THROW(source.start({}, recorder.callbacks()), std::invalid_argument);
    EXPECT_THROW(source.start({Item{.id = 1, .locator = "site01.line01.comp01/unknown"}},
                              recorder.callbacks()),
                 std::invalid_argument);
    const auto locator = modbus_point_locator(device_config().points[0]);
    EXPECT_THROW(
        source.start({Item{1, locator}, Item{1, modbus_point_locator(device_config().points[1])}},
                     recorder.callbacks()),
        std::invalid_argument); // duplicate ID
    EXPECT_THROW(source.start({Item{1, locator}, Item{2, locator}}, recorder.callbacks()),
                 std::invalid_argument); // duplicate locator
    EXPECT_THROW(source.start({Item{1, locator}}, acquisition::Callbacks{}), std::invalid_argument);
    EXPECT_EQ(harness.device().clients_alive(), 0);
    harness.start(); // still startable
    EXPECT_TRUE(harness.recorder().wait_for_updates(5));
}

TEST(ModbusTcpSource, StartsAtMostOnceAndNeverAfterStop)
{
    SourceHarness harness;
    harness.start();
    EXPECT_THROW(harness.start(), std::logic_error);
    harness.source().stop();
    SourceHarness stopped;
    stopped.source().stop();
    EXPECT_THROW(stopped.start(), std::logic_error);
}

TEST(ModbusTcpSource, StopIsSafeBeforeStartAndIdempotent)
{
    SourceHarness harness;
    harness.source().stop();
    harness.source().stop();
    SourceHarness started;
    started.start();
    started.source().stop();
    started.source().stop();
}

// ---- Successful polling ----------------------------------------------------------------------

TEST(ModbusTcpSource, StartDoesNotWaitForTheConnection)
{
    SourceHarness harness;
    harness.device().fail_next_connects(1000);
    const auto before = std::chrono::steady_clock::now();
    harness.start();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 100ms);
    EXPECT_TRUE(harness.recorder().wait_for_state(ConnectionState::Disconnected));
}

TEST(ModbusTcpSource, FirstCycleDeliversTypedGoodUpdatesForEveryItem)
{
    SourceHarness harness;
    harness.start();
    ASSERT_TRUE(harness.recorder().wait_for_updates(5));
    const auto updates = harness.recorder().updates();
    EXPECT_EQ(last_for(updates, 100), good(100, 7.5));
    EXPECT_EQ(last_for(updates, 101), good(101, std::int64_t{-100}));
    EXPECT_EQ(last_for(updates, 102), good(102, kFloatPi));
    EXPECT_EQ(last_for(updates, 103), good(103, std::uint64_t{42}));
    EXPECT_EQ(last_for(updates, 104), good(104, true));
    const auto statuses = harness.recorder().statuses();
    ASSERT_GE(statuses.size(), 2U);
    EXPECT_EQ(statuses[0].state, ConnectionState::Connecting);
    EXPECT_EQ(statuses[1].state, ConnectionState::Connected);
    EXPECT_EQ(statuses[1].source_id, kSource);
}

TEST(ModbusTcpSource, CoalescesContiguousPointsAndKeepsGapsSeparate)
{
    SourceHarness harness;
    harness.start();
    ASSERT_TRUE(harness.recorder().wait_for_updates(5));
    EXPECT_EQ(harness.device().requests(),
              (std::vector<Request>{{ModbusTable::Coils, 0, 1},
                                    {ModbusTable::HoldingRegisters, 0, 4},
                                    {ModbusTable::HoldingRegisters, 10, 1}}));
}

TEST(ModbusTcpSource, PollsOnlyTheRequestedSubsetOfPoints)
{
    SourceHarness harness;
    harness.start({all_items()[3]});
    ASSERT_TRUE(harness.recorder().wait_for_updates(1));
    harness.wait_for_cycle_waits(1);
    EXPECT_EQ(harness.device().requests(),
              (std::vector<Request>{{ModbusTable::HoldingRegisters, 10, 1}}));
}

TEST(ModbusTcpSource, PollsOnAFixedGridOfPollIntervals)
{
    SourceHarness harness;
    harness.start();
    harness.wait_for_cycle_waits(1);
    for (std::size_t cycle = 2; cycle <= 4; ++cycle) {
        harness.clock().advance_to_deadline();
        harness.wait_for_cycle_waits(cycle);
    }
    const auto deadlines = harness.clock().deadlines();
    const auto t0 = ManualClock::start();
    EXPECT_EQ(deadlines,
              (std::vector<ManualClock::time_point>{t0 + kInterval, t0 + 2 * kInterval,
                                                    t0 + 3 * kInterval, t0 + 4 * kInterval}));
    EXPECT_EQ(harness.device().requests().size(), 4U * 3U);
    EXPECT_EQ(harness.recorder().updates().size(), 4U * 5U);
}

TEST(ModbusTcpSource, SkipsMissedSlotsWhenACycleOverruns)
{
    SourceHarness harness;
    int reads = 0;
    // The fourth read (second cycle, first block) takes 2.5 intervals of virtual time.
    harness.device().on_read([&] {
        if (++reads == 4) {
            harness.clock().advance(kInterval * 5 / 2);
        }
    });
    harness.start();
    harness.wait_for_cycle_waits(1);
    harness.clock().advance_to_deadline();
    harness.wait_for_cycle_waits(2);
    const auto t0 = ManualClock::start();
    // Second cycle started at t0 + 1 s and ended at t0 + 3.5 s: slots 2 s and 3 s are skipped.
    EXPECT_EQ(harness.clock().last_deadline(), t0 + 4 * kInterval);
}

TEST(ModbusTcpSource, NeverSendsConcurrentRequests)
{
    SourceHarness harness;
    harness.start();
    harness.wait_for_cycle_waits(1);
    harness.clock().advance_to_deadline();
    harness.wait_for_cycle_waits(2);
    EXPECT_EQ(harness.device().max_concurrent_requests(), 1);
}

// ---- Failures --------------------------------------------------------------------------------

TEST(ModbusTcpSource, SplitsABlockThatAnswersWithAnExceptionAndKeepsTheNeighboursGood)
{
    SourceHarness harness;
    harness.device().set_exception(ModbusTable::HoldingRegisters, 1, 1, 0x02);
    harness.start();
    harness.wait_for_cycle_waits(1);
    const auto updates = harness.recorder().updates();
    EXPECT_EQ(last_for(updates, 100), good(100, 7.5));
    EXPECT_EQ(last_for(updates, 101), bad(101, "modbus_exception", 0x02));
    EXPECT_EQ(last_for(updates, 102), good(102, kFloatPi));
    const auto first_cycle = harness.device().requests();
    EXPECT_EQ(first_cycle,
              (std::vector<Request>{{ModbusTable::Coils, 0, 1},
                                    {ModbusTable::HoldingRegisters, 0, 4}, // answered 0x02
                                    {ModbusTable::HoldingRegisters, 0, 1},
                                    {ModbusTable::HoldingRegisters, 1, 1},
                                    {ModbusTable::HoldingRegisters, 2, 2},
                                    {ModbusTable::HoldingRegisters, 10, 1}}));
    // Later cycles keep the split instead of retrying the large block.
    harness.clock().advance_to_deadline();
    harness.wait_for_cycle_waits(2);
    const auto all = harness.device().requests();
    EXPECT_EQ(std::vector<Request>(all.begin() + 6, all.end()),
              (std::vector<Request>{{ModbusTable::Coils, 0, 1},
                                    {ModbusTable::HoldingRegisters, 0, 1},
                                    {ModbusTable::HoldingRegisters, 1, 1},
                                    {ModbusTable::HoldingRegisters, 2, 2},
                                    {ModbusTable::HoldingRegisters, 10, 1}}));
    EXPECT_EQ(harness.device().connects(), 1); // exceptions keep the connection
}

TEST(ModbusTcpSource, ReportsAnExceptionOfASinglePointReadWithItsCode)
{
    SourceHarness harness;
    harness.device().set_exception(ModbusTable::HoldingRegisters, 10, 10, 0x04);
    harness.start();
    harness.wait_for_cycle_waits(1);
    EXPECT_EQ(last_for(harness.recorder().updates(), 103), bad(103, "modbus_exception", 0x04));
}

TEST(ModbusTcpSource, ReportsDecodeFailuresWithoutANativeStatus)
{
    SourceHarness harness;
    harness.device().set_register(ModbusTable::HoldingRegisters, 2, 0x0000); // 0x7FC00000: NaN
    harness.device().set_register(ModbusTable::HoldingRegisters, 3, 0x7FC0);
    harness.start();
    harness.wait_for_cycle_waits(1);
    const auto updates = harness.recorder().updates();
    EXPECT_EQ(last_for(updates, 102), bad(102, "decode_non_finite_value"));
    EXPECT_EQ(last_for(updates, 100), good(100, 7.5));
}

TEST(ModbusTcpSource, ATimeoutMarksTheRestOfTheCycleBadAndReconnectsAfterBackoff)
{
    SourceHarness harness;
    harness.start();
    harness.wait_for_cycle_waits(1);
    // Second cycle: the coil read times out (the fake client closes like the real one).
    harness.device().fail_next_read(error_of(ModbusErrorKind::Timeout));
    harness.clock().advance_to_deadline();
    ASSERT_TRUE(harness.recorder().wait_for_state(ConnectionState::Disconnected));
    harness.wait_for_cycle_waits(2); // backoff wait
    EXPECT_TRUE(all_bad(harness.recorder().updates(), {100, 101, 102, 103, 104}, "timeout"));
    EXPECT_EQ(harness.clock().last_deadline(),
              ManualClock::start() + kInterval + 100ms); // initial backoff
    EXPECT_TRUE(harness.recorder().last_status().detail.starts_with("timeout: "));

    harness.clock().advance_to_deadline();
    ASSERT_TRUE(harness.recorder().wait_for_state(ConnectionState::Connected, 2));
    harness.wait_for_cycle_waits(3);
    EXPECT_EQ(last_for(harness.recorder().updates(), 100), good(100, 7.5)); // recovered
    EXPECT_EQ(harness.device().connects(), 2);
}

TEST(ModbusTcpSource, ConnectionLossReportsDisconnectedWithAStableDiagnostic)
{
    SourceHarness harness;
    harness.device().fail_next_read(error_of(ModbusErrorKind::ConnectionLost));
    harness.start();
    ASSERT_TRUE(harness.recorder().wait_for_state(ConnectionState::Disconnected));
    harness.wait_for_cycle_waits(1);
    const auto updates = harness.recorder().updates();
    EXPECT_EQ(last_for(updates, 104), bad(104, "connection_lost"));
    EXPECT_EQ(last_for(updates, 103), bad(103, "connection_lost"));
    EXPECT_TRUE(
        std::ranges::none_of(updates, [](const Update& u) { return u.native_status.has_value(); }));
}

TEST(ModbusTcpSource, BacksOffExponentiallyAndResetsAfterAConnection)
{
    SourceHarness harness;
    harness.device().fail_next_connects(4);
    harness.start();
    release_waits(harness.clock(), 5); // four backoff waits, then the first poll wait
    // A later loss starts again from the initial delay.
    harness.device().fail_next_read(error_of(ModbusErrorKind::ConnectionLost));
    harness.clock().advance_to_deadline();
    harness.wait_for_cycle_waits(6);
    const auto t0 = ManualClock::start();
    // Backoffs of 100, 200, 400 and 400 ms (max), each measured from the failed attempt; the
    // connection succeeds at 1100 ms, polls until 2100 ms, then waits the initial 100 ms again.
    EXPECT_EQ(harness.clock().deadlines(),
              (std::vector<ManualClock::time_point>{t0 + 100ms, t0 + 300ms, t0 + 700ms, t0 + 1100ms,
                                                    t0 + 1100ms + kInterval,
                                                    t0 + 1100ms + kInterval + 100ms}));
    EXPECT_EQ(last_for(harness.recorder().updates(), 100), bad(100, "connection_lost"));
}

TEST(ModbusTcpSource, JitterIsBoundedAndDeterministicForAFixedSeed)
{
    const auto first = reconnect_delays(7, 0.5, 5);
    EXPECT_EQ(first, reconnect_delays(7, 0.5, 5));
    EXPECT_TRUE(within_jitter(first, {100ms, 200ms, 400ms, 400ms, 400ms}, 0.5));
    EXPECT_NE(first, reconnect_delays(8, 0.5, 5)); // another seed, another sequence
}

TEST(ModbusTcpSource, RetriesWhenTheClientFactoryFails)
{
    SourceHarness harness;
    harness.device().throw_on_next_client(1);
    harness.start();
    ASSERT_TRUE(harness.recorder().wait_for_state(ConnectionState::Disconnected));
    EXPECT_EQ(harness.recorder().statuses().front().detail, "client_error: cannot create client");
    harness.wait_for_cycle_waits(1);
    harness.clock().advance_to_deadline();
    EXPECT_TRUE(harness.recorder().wait_for_updates(5));
}

// ---- Threading and shutdown --------------------------------------------------------------------

TEST(ModbusTcpSource, TheClientLivesOnlyOnOneWorkerThread)
{
    SourceHarness harness;
    harness.start();
    harness.wait_for_cycle_waits(1);
    EXPECT_EQ(harness.device().clients_alive(), 1);
    harness.source().stop();
    EXPECT_EQ(harness.device().clients_alive(), 0); // destroyed before stop() returned
    const auto threads = harness.device().client_threads();
    ASSERT_EQ(threads.size(), 1U); // created, used, closed and destroyed there
    EXPECT_NE(*threads.begin(), std::this_thread::get_id());
}

TEST(ModbusTcpSource, NoCallbackRunsAfterStopReturns)
{
    SourceHarness harness;
    harness.start();
    harness.wait_for_cycle_waits(1);
    const auto before = std::chrono::steady_clock::now();
    harness.source().stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 500ms); // the poll wait ends at once
    const auto total = harness.recorder().total();
    harness.clock().advance(10 * kInterval);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(harness.recorder().total(), total);
    EXPECT_NE(harness.recorder().last_status().state, ConnectionState::Disconnected);
}

TEST(ModbusTcpSource, StopDuringBackoffReturnsPromptly)
{
    SourceHarness harness;
    harness.device().fail_next_connects(1000);
    harness.start();
    harness.wait_for_cycle_waits(1);
    const auto before = std::chrono::steady_clock::now();
    harness.source().stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 500ms);
}

TEST(ModbusTcpSource, DestructionStopsTheWorker)
{
    SourceHarness harness;
    harness.start();
    harness.wait_for_cycle_waits(1);
    harness.destroy();
    EXPECT_EQ(harness.device().clients_alive(), 0);
}

} // namespace
} // namespace ieg::modbus
