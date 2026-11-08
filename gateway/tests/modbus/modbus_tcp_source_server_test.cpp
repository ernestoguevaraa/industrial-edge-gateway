// ModbusTcpSource with the real LibModbusClient and steady clock against the deterministic
// in-process Modbus TCP server (127.0.0.1, ephemeral port): conversions, exceptions, connection
// loss and recovery, and measured stop latency.

#include "ieg/modbus/modbus_tcp_source.hpp"
#include "modbus/modbus_test_server.hpp"
#include "modbus/source_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
using testing::ModbusTestServer;
using testing::SourceRecorder;
using Mode = ModbusTestServer::Mode;
using Clock = std::chrono::steady_clock;

constexpr auto kPollInterval = 50ms;
constexpr auto kRequestTimeout = 300ms;

config::ModbusPointConfig point(const std::string& tag, ModbusTable table, std::uint16_t offset,
                                ModbusDataType type, std::optional<ModbusWordOrder> order = {},
                                std::optional<double> scale = {})
{
    config::ModbusPointConfig result;
    result.asset = "site01.line01.comp01";
    result.tag = tag;
    result.table = table;
    result.offset = offset;
    result.type = type;
    result.word_order = order;
    result.scale = scale;
    return result;
}

config::ModbusDeviceConfig device_for(std::uint16_t port)
{
    config::ModbusDeviceConfig device;
    device.source_id = "compressor-plc";
    device.host = "127.0.0.1";
    device.port = port;
    device.unit_id = 3;
    device.poll_interval = kPollInterval;
    device.timeout = kRequestTimeout;
    device.points = {
        point("outlet_pressure", ModbusTable::HoldingRegisters, 0, ModbusDataType::UInt16, {}, 0.1),
        point("ambient", ModbusTable::InputRegisters, 0, ModbusDataType::Int16, {}, 0.5),
        point("run_hours", ModbusTable::InputRegisters, 1, ModbusDataType::UInt32,
              ModbusWordOrder::HighWordFirst),
        point("motor_current", ModbusTable::HoldingRegisters, 10, ModbusDataType::Float32,
              ModbusWordOrder::LowWordFirst),
        point("running", ModbusTable::Coils, 0, ModbusDataType::Bool),
        point("door_open", ModbusTable::DiscreteInputs, 7, ModbusDataType::Bool),
    };
    return device;
}

std::vector<Item> items_for(const config::ModbusDeviceConfig& device)
{
    std::vector<Item> items;
    for (std::size_t i = 0; i < device.points.size(); ++i) {
        items.push_back(Item{.id = static_cast<acquisition::ItemId>(i + 1),
                             .locator = modbus_point_locator(device.points[i])});
    }
    return items;
}

ModbusSourceOptions fast_reconnect()
{
    return ModbusSourceOptions{.reconnect_initial_delay = 50ms,
                               .reconnect_max_delay = 200ms,
                               .reconnect_jitter = 0.0,
                               .jitter_seed = 1,
                               .max_registers_per_read = kMaxReadRegisters,
                               .max_bits_per_read = kMaxReadBits};
}

std::optional<Update> last_for(const std::vector<Update>& updates, acquisition::ItemId item)
{
    const auto found = std::ranges::find_if(updates.rbegin(), updates.rend(),
                                            [&](const Update& u) { return u.item_id == item; });
    return found == updates.rend() ? std::nullopt : std::optional{*found};
}

// Latest value of items 1..count (nullopt for an item without a valued update).
std::vector<std::optional<domain::TelemetryValue>> latest_values(const std::vector<Update>& updates,
                                                                 std::size_t count)
{
    std::vector<std::optional<domain::TelemetryValue>> values;
    for (acquisition::ItemId item = 1; item <= count; ++item) {
        const auto latest = last_for(updates, item);
        values.push_back(latest ? latest->value : std::nullopt);
    }
    return values;
}

// Latest diagnostic and native status of `item` ("<none>" if it has no update).
std::string latest_diagnostic(const std::vector<Update>& updates, acquisition::ItemId item)
{
    const auto latest = last_for(updates, item);
    return latest ? latest->diagnostic : std::string{"<none>"};
}

std::optional<acquisition::NativeStatusCode> latest_native(const std::vector<Update>& updates,
                                                           acquisition::ItemId item)
{
    const auto latest = last_for(updates, item);
    return latest ? latest->native_status : std::nullopt;
}

// Waits until every item's latest update has quality `quality`.
bool all_latest(SourceRecorder& recorder, std::size_t items, DataQuality quality)
{
    return recorder.wait_until([&](const auto& updates, const auto&) {
        for (acquisition::ItemId item = 1; item <= items; ++item) {
            const auto latest = last_for(updates, item);
            if (!latest || latest->quality != quality) {
                return false;
            }
        }
        return true;
    });
}

class ModbusSourceServerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server_.set_holding(0, 75);   // 7.5 bar
        server_.set_input(0, 0xFFEC); // -20 * 0.5 = -10.0
        server_.set_input(1, 0x0001); // 0x0001'86A0 = 100000 h
        server_.set_input(2, 0x86A0);
        server_.set_holding(10, 0x0000); // float32 2.5 = 0x40200000, low word first
        server_.set_holding(11, 0x4020);
        server_.set_coil(0, true);
        server_.set_discrete(7, true);
        device_ = device_for(server_.port());
        source_ = std::make_unique<ModbusTcpSource>(device_, fast_reconnect());
    }

    void start() { source_->start(items_for(device_), recorder_.callbacks()); }

    ModbusTestServer& server() { return server_; }
    SourceRecorder& recorder() { return recorder_; }
    ModbusTcpSource& source() { return *source_; }

private:
    ModbusTestServer server_;
    config::ModbusDeviceConfig device_;
    SourceRecorder recorder_;
    std::unique_ptr<ModbusTcpSource> source_;
};

TEST_F(ModbusSourceServerTest, ReadsAndConvertsEveryTableThroughTheRealClient)
{
    start();
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Good));
    source().stop();
    const auto updates = recorder().updates();
    const std::vector<std::optional<domain::TelemetryValue>> expected{
        7.5, -10.0, std::uint64_t{100'000}, 2.5, true, true};
    EXPECT_EQ(latest_values(updates, 6), expected);
    EXPECT_TRUE(std::ranges::all_of(updates, [](const Update& u) {
        return u.source_id == "compressor-plc" && !u.source_timestamp && !u.native_status;
    }));
    EXPECT_EQ(server().last_unit_id(), 3);
}

TEST_F(ModbusSourceServerTest, ReportsExceptionCodesAndRecoversOnTheSameConnection)
{
    start();
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Good));
    server().set_mode(Mode::Exception, 0x02);
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Bad));
    EXPECT_EQ(latest_native(recorder().updates(), 1),
              std::optional<acquisition::NativeStatusCode>{0x02});
    EXPECT_EQ(latest_diagnostic(recorder().updates(), 1), "modbus_exception");
    server().set_mode(Mode::Normal);
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Good));
    EXPECT_EQ(server().accepted(), 1);
}

TEST_F(ModbusSourceServerTest, RecoversFromAConnectionLoss)
{
    start();
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Good));
    server().set_mode(Mode::CloseOnRequest);
    ASSERT_TRUE(recorder().wait_for_state(ConnectionState::Disconnected));
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Bad));
    EXPECT_EQ(latest_diagnostic(recorder().updates(), 1), "connection_lost");
    server().set_mode(Mode::Normal);
    ASSERT_TRUE(recorder().wait_for_state(ConnectionState::Connected, 2));
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Good));
    EXPECT_GE(server().accepted(), 2);
}

TEST_F(ModbusSourceServerTest, ReportsTimeoutsAndReconnects)
{
    start();
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Good));
    server().set_mode(Mode::Silent);
    ASSERT_TRUE(recorder().wait_for_state(ConnectionState::Disconnected));
    EXPECT_EQ(latest_diagnostic(recorder().updates(), 1), "timeout");
    server().set_mode(Mode::Normal);
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Good));
}

TEST_F(ModbusSourceServerTest, StopWhileARequestIsBlockedWaitsAtMostOneRequestTimeout)
{
    start();
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Good));
    server().set_mode(Mode::Silent);
    const int before = server().requests();
    ASSERT_TRUE(ModbusTestServer::wait_until([&] { return server().requests() > before; }));
    const auto stop_started = Clock::now();
    source().stop();
    const auto took = Clock::now() - stop_started;
    RecordProperty(
        "stop_latency_blocked_request_ms",
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
    EXPECT_LT(took, kRequestTimeout + 200ms);
    const auto total = recorder().total();
    std::this_thread::sleep_for(3 * kPollInterval);
    EXPECT_EQ(recorder().total(), total); // nothing after stop() returned
}

TEST_F(ModbusSourceServerTest, StopWhileIdleIsImmediate)
{
    start();
    ASSERT_TRUE(all_latest(recorder(), 6, DataQuality::Good));
    const auto stop_started = Clock::now();
    source().stop();
    const auto took = Clock::now() - stop_started;
    RecordProperty(
        "stop_latency_idle_ms",
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
    EXPECT_LT(took, kRequestTimeout); // usually well below one millisecond plus one request
    EXPECT_TRUE(ModbusTestServer::wait_until([&] { return server().closed_by_peer() >= 1; }));
}

TEST(ModbusSourceUnavailableServer, KeepsRetryingWithoutUpdatesAndStopsPromptly)
{
    const auto port = [] {
        const ModbusTestServer probe; // bound then released when the probe is destroyed
        return probe.port();
    }();
    const auto device = device_for(port);
    ModbusTcpSource source{device, fast_reconnect()};
    SourceRecorder recorder;
    source.start(items_for(device), recorder.callbacks());
    ASSERT_TRUE(recorder.wait_for_state(ConnectionState::Disconnected, 3));
    const auto stop_started = Clock::now();
    source.stop();
    EXPECT_LT(Clock::now() - stop_started, kRequestTimeout + 200ms);
    EXPECT_TRUE(recorder.updates().empty()); // no data without a connection, only statuses
    EXPECT_TRUE(recorder.last_status().detail.starts_with("connection_failed: "));
}

} // namespace
} // namespace ieg::modbus
