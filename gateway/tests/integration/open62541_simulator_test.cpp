// Integration tests: the real Open62541Client against the real Python plant simulator.
// Skipped unless IEG_SIMULATOR_PYTHON names an interpreter with the simulator requirements.

#include "ieg/opcua/open62541_client.hpp"
#include "integration/simulator_process.hpp"
#include "opcua/callback_recorder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace ieg::opcua {
namespace {

using namespace std::chrono_literals;
using domain::DataQuality;
using ieg::testing::SimulatorProcess;
using testing::CallbackRecorder;

constexpr StatusCode kBadNodeIdUnknown = 0x80340000U;

enum class Kind : std::uint8_t
{
    String,
    Double,
    Unsigned
};

struct Expected
{
    ItemId id;
    const char* node_id;
    Kind kind;
};

// The documented MVP 1 nodes (configs/gateway.example.yaml, simulator/README.md).
constexpr std::array<Expected, 7> kDocumentedNodes{{
    {1, "ns=2;s=Line01.Press01.State", Kind::String},
    {2, "ns=2;s=Line01.Press01.Pressure", Kind::Double},
    {3, "ns=2;s=Line01.Press01.CycleTime", Kind::Double},
    {4, "ns=2;s=Line01.Press01.ProductionCount", Kind::Unsigned},
    {5, "ns=2;s=Line01.Oven01.State", Kind::String},
    {6, "ns=2;s=Line01.Oven01.Temperature", Kind::Double},
    {7, "ns=2;s=Line01.Oven01.ProductionCount", Kind::Unsigned},
}};
constexpr ItemId kUnknownItem = 99;

std::vector<MonitoredItem> documented_items()
{
    std::vector<MonitoredItem> items;
    items.reserve(kDocumentedNodes.size());
    for (const auto& node : kDocumentedNodes) {
        items.push_back({node.id, node.node_id});
    }
    return items;
}

bool has_kind(const domain::TelemetryValue& value, Kind kind)
{
    switch (kind) {
    case Kind::String:
        return std::holds_alternative<std::string>(value);
    case Kind::Double:
        return std::holds_alternative<double>(value);
    case Kind::Unsigned:
        return std::holds_alternative<std::uint64_t>(value);
    }
    return false;
}

// Number of distinct item ids with at least one Good update at or after `from_index`.
std::size_t good_items_since(const CallbackRecorder& r, std::size_t from_index)
{
    std::set<ItemId> ids;
    const auto& data = r.data_unlocked();
    for (std::size_t i = from_index; i < data.size(); ++i) {
        if (data[i].quality == DataQuality::Good && data[i].id != kUnknownItem) {
            ids.insert(data[i].id);
        }
    }
    return ids.size();
}

::testing::AssertionResult connected_without_failures(const std::vector<ConnectionStatus>& statuses)
{
    const std::vector<ConnectionStatus> expected_prefix{{ConnectionState::Connecting, ""},
                                                        {ConnectionState::Connected, ""}};
    if (statuses.size() < 2 ||
        !std::equal(expected_prefix.begin(), expected_prefix.end(), statuses.begin())) {
        return ::testing::AssertionFailure() << "expected Connecting, Connected first";
    }
    if (std::ranges::any_of(statuses, [](const ConnectionStatus& s) {
            return s.state == ConnectionState::Disconnected;
        })) {
        return ::testing::AssertionFailure() << "unexpected Disconnected";
    }
    return ::testing::AssertionSuccess();
}

std::map<ItemId, DataChange> first_good_updates(const std::vector<DataChange>& data)
{
    std::map<ItemId, DataChange> first_good;
    for (const auto& change : data) {
        if (change.quality == DataQuality::Good) {
            first_good.try_emplace(change.id, change);
        }
    }
    return first_good;
}

::testing::AssertionResult is_good_typed_update(const DataChange& change, const Expected& node,
                                                std::chrono::system_clock::time_point now)
{
    if (change.status_code != 0U) {
        return ::testing::AssertionFailure() << node.node_id << ": non-Good status";
    }
    if (!change.value || !has_kind(*change.value, node.kind)) {
        return ::testing::AssertionFailure() << node.node_id << ": missing or mistyped value";
    }
    if (!change.source_timestamp) {
        return ::testing::AssertionFailure() << node.node_id << ": no source timestamp";
    }
    if (std::chrono::abs(now - *change.source_timestamp) > 60s) {
        return ::testing::AssertionFailure() << node.node_id << ": implausible source timestamp";
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult is_unknown_node_failure(const DataChange& change)
{
    if (change.value || change.quality != DataQuality::Bad ||
        change.status_code != kBadNodeIdUnknown || change.source_timestamp) {
        return ::testing::AssertionFailure()
               << "expected Bad/BadNodeIdUnknown without value or timestamp";
    }
    return ::testing::AssertionSuccess();
}

Open62541ClientOptions options_for(const std::string& endpoint)
{
    return Open62541ClientOptions{.endpoint = endpoint,
                                  .publishing_interval = 200ms,
                                  .request_timeout = 2000ms,
                                  .reconnect_initial_delay = 200ms,
                                  .reconnect_max_delay = 1000ms};
}

class SimulatorIntegration : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto python = SimulatorProcess::python_from_environment();
        if (!python) {
            GTEST_SKIP() << "IEG_SIMULATOR_PYTHON is not set; skipping simulator integration";
        }
        simulator_ = std::make_unique<SimulatorProcess>(*python, SimulatorProcess::free_port());
        simulator_->launch();
    }

    [[nodiscard]] SimulatorProcess& simulator() const { return *simulator_; }

private:
    std::unique_ptr<SimulatorProcess> simulator_;
};

TEST_F(SimulatorIntegration, ReceivesAllDocumentedNodesWithTypesAndSourceTimestamps)
{
    // The simulator binds 0.0.0.0 and advertises opc.tcp://0.0.0.0:<port>/; connecting via
    // 127.0.0.1 proves the advertised address does not break session establishment.
    CallbackRecorder recorder;
    Open62541Client client{options_for(simulator().endpoint())};
    client.start(documented_items(), recorder.callbacks());

    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) { return good_items_since(r, 0) == kDocumentedNodes.size(); },
        15s));
    client.stop();

    EXPECT_TRUE(connected_without_failures(recorder.statuses()));

    const auto now = std::chrono::system_clock::now();
    const auto first_good = first_good_updates(recorder.data());
    for (const auto& node : kDocumentedNodes) {
        EXPECT_TRUE(is_good_typed_update(first_good.at(node.id), node, now));
    }
    EXPECT_FALSE(recorder.overlap_detected());
}

TEST_F(SimulatorIntegration, UnknownNodeIsBadWithoutValueWhileOthersStayGood)
{
    CallbackRecorder recorder;
    Open62541Client client{options_for(simulator().endpoint())};
    auto items = documented_items();
    items.push_back({kUnknownItem, "ns=2;s=Line01.DoesNotExist"});
    client.start(items, recorder.callbacks());

    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) {
            const bool unknown_reported = std::ranges::any_of(
                r.data_unlocked(), [](const DataChange& c) { return c.id == kUnknownItem; });
            return unknown_reported && good_items_since(r, 0) == kDocumentedNodes.size();
        },
        15s));
    client.stop();

    for (const auto& change : recorder.data()) {
        if (change.id == kUnknownItem) {
            EXPECT_TRUE(is_unknown_node_failure(change));
        }
    }
}

TEST_F(SimulatorIntegration, ReconnectsAndRestoresEveryMonitoredItemAfterSimulatorRestart)
{
    CallbackRecorder recorder;
    Open62541Client client{options_for(simulator().endpoint())};
    client.start(documented_items(), recorder.callbacks());
    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) { return good_items_since(r, 0) == kDocumentedNodes.size(); },
        15s));

    simulator().terminate();
    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) {
            return r.count_state_unlocked(ConnectionState::Disconnected) >= 1;
        },
        15s));
    const std::size_t data_before_restart = recorder.data().size();
    const std::size_t connected_before = recorder.count_state(ConnectionState::Connected);

    simulator().launch();
    ASSERT_TRUE(recorder.wait_until(
        [&](const CallbackRecorder& r) {
            return r.count_state_unlocked(ConnectionState::Connected) > connected_before &&
                   good_items_since(r, data_before_restart) == kDocumentedNodes.size();
        },
        30s));
    client.stop();
    EXPECT_FALSE(recorder.overlap_detected());
}

TEST_F(SimulatorIntegration, StopWhileConnectedIsSilentAndFinal)
{
    CallbackRecorder recorder;
    Open62541Client client{options_for(simulator().endpoint())};
    client.start(documented_items(), recorder.callbacks());
    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) { return good_items_since(r, 0) == kDocumentedNodes.size(); },
        15s));

    const auto before = std::chrono::steady_clock::now();
    client.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2500ms);

    EXPECT_EQ(recorder.active_callbacks(), 0);
    const auto calls = recorder.total_calls();
    std::this_thread::sleep_for(700ms); // > several publishing intervals
    EXPECT_EQ(recorder.total_calls(), calls);
    EXPECT_EQ(recorder.count_state(ConnectionState::Disconnected), 0U);
}

TEST_F(SimulatorIntegration, StopWhileBackingOffIsSilentAndFinal)
{
    CallbackRecorder recorder;
    auto options = options_for(simulator().endpoint());
    options.reconnect_initial_delay = 30s;
    options.reconnect_max_delay = 30s;
    Open62541Client client{options};
    client.start(documented_items(), recorder.callbacks());
    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) {
            return r.count_state_unlocked(ConnectionState::Connected) == 1;
        },
        15s));
    simulator().terminate();
    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) {
            return r.count_state_unlocked(ConnectionState::Disconnected) == 1;
        },
        15s));

    const auto before = std::chrono::steady_clock::now();
    client.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 500ms);
    const auto calls = recorder.total_calls();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(recorder.total_calls(), calls);
    EXPECT_EQ(recorder.count_state(ConnectionState::Disconnected), 1U);
}

} // namespace
} // namespace ieg::opcua
