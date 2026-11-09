// Integration tests: the OpcUaSource acquisition bridge over the real Open62541Client against the
// real Python plant simulator. Skipped unless IEG_SIMULATOR_PYTHON is set.

#include "ieg/acquisition/opcua_source.hpp"
#include "ieg/opcua/open62541_client.hpp"
#include "integration/simulator_process.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace ieg::acquisition {
namespace {

using namespace std::chrono_literals;
using ieg::testing::SimulatorProcess;

constexpr const char* kSourceId = "opcua-main";
constexpr NativeStatusCode kBadNodeIdUnknown = 0x80340000U;
constexpr ItemId kUnknownItem = 99;

std::vector<Item> items_with_unknown_node()
{
    return {{1, "ns=2;s=Line01.Press01.State"},
            {2, "ns=2;s=Line01.Press01.Pressure"},
            {6, "ns=2;s=Line01.Oven01.Temperature"},
            {kUnknownItem, "ns=2;s=Line01.DoesNotExist"}};
}

// Thread-safe sink for source callbacks (they arrive on the client's worker thread).
class Recorder
{
public:
    Callbacks callbacks()
    {
        return Callbacks{.on_update = [this](const Update& u) { record(updates_, u); },
                         .on_status = [this](const Status& s) { record(statuses_, s); }};
    }

    template <typename Predicate>
    bool wait_until(Predicate predicate, std::chrono::seconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(updates_, statuses_); });
    }

    [[nodiscard]] std::vector<Update> updates() const
    {
        const std::lock_guard lock(mutex_);
        return updates_;
    }

    [[nodiscard]] std::vector<Status> statuses() const
    {
        const std::lock_guard lock(mutex_);
        return statuses_;
    }

    [[nodiscard]] std::size_t total_calls() const
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
    std::vector<Update> updates_;
    std::vector<Status> statuses_;
};

bool all_known_items_good_and_unknown_reported(const std::vector<Update>& updates,
                                               const std::vector<Status>& /*statuses*/)
{
    std::set<ItemId> good;
    bool unknown = false;
    for (const auto& update : updates) {
        unknown = unknown || update.item_id == kUnknownItem;
        if (update.quality == domain::DataQuality::Good && update.item_id != kUnknownItem) {
            good.insert(update.item_id);
        }
    }
    return unknown && good.size() == 3;
}

::testing::AssertionResult is_good_with_status_and_timestamp(const Update& update)
{
    if (update.source_id != kSourceId || !update.value || !update.source_timestamp ||
        update.native_status != std::optional<NativeStatusCode>{0U}) {
        return ::testing::AssertionFailure()
               << "item " << update.item_id << ": expected Good value, timestamp, status 0";
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult is_unknown_node(const Update& update)
{
    if (update.source_id != kSourceId || update.value ||
        update.quality != domain::DataQuality::Bad || update.source_timestamp ||
        update.native_status != std::optional<NativeStatusCode>{kBadNodeIdUnknown}) {
        return ::testing::AssertionFailure()
               << "expected Bad without value/timestamp and native status BadNodeIdUnknown";
    }
    return ::testing::AssertionSuccess();
}

// Unknown node: Bad, no value/timestamp, raw BadNodeIdUnknown. Good updates: value, timestamp, 0.
::testing::AssertionResult updates_preserve_opcua_semantics(const std::vector<Update>& updates)
{
    for (const auto& update : updates) {
        auto result = ::testing::AssertionSuccess();
        if (update.item_id == kUnknownItem) {
            result = is_unknown_node(update);
        } else if (update.quality == domain::DataQuality::Good) {
            result = is_good_with_status_and_timestamp(update);
        }
        if (!result) {
            return result;
        }
    }
    return ::testing::AssertionSuccess();
}

class OpcUaSourceSimulator : public ::testing::Test
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

    [[nodiscard]] std::unique_ptr<OpcUaSource> make_source() const
    {
        return std::make_unique<OpcUaSource>(
            kSourceId, std::make_unique<opcua::Open62541Client>(
                           opcua::Open62541ClientOptions{.endpoint = simulator_->endpoint(),
                                                         .publishing_interval = 200ms,
                                                         .request_timeout = 2000ms,
                                                         .reconnect_initial_delay = 200ms,
                                                         .reconnect_max_delay = 1000ms}));
    }

private:
    std::unique_ptr<SimulatorProcess> simulator_;
};

TEST_F(OpcUaSourceSimulator, DeliversValuesTimestampsAndRawStatusCodesThroughTheBridge)
{
    Recorder recorder;
    const auto source = make_source();
    source->start(items_with_unknown_node(), recorder.callbacks());
    ASSERT_TRUE(recorder.wait_until(all_known_items_good_and_unknown_reported, 15s));
    source->stop();

    EXPECT_TRUE(updates_preserve_opcua_semantics(recorder.updates()));
    const auto statuses = recorder.statuses();
    ASSERT_GE(statuses.size(), 2U);
    EXPECT_EQ(statuses[0], (Status{kSourceId, ConnectionState::Connecting, ""}));
    EXPECT_EQ(statuses[1], (Status{kSourceId, ConnectionState::Connected, ""}));
}

TEST_F(OpcUaSourceSimulator, StopIsFinalAndSilent)
{
    Recorder recorder;
    const auto source = make_source();
    source->start(items_with_unknown_node(), recorder.callbacks());
    ASSERT_TRUE(recorder.wait_until(all_known_items_good_and_unknown_reported, 15s));

    source->stop();
    const auto calls = recorder.total_calls();
    std::this_thread::sleep_for(700ms); // > several publishing intervals
    EXPECT_EQ(recorder.total_calls(), calls);
    const auto statuses = recorder.statuses();
    EXPECT_TRUE(std::ranges::none_of(statuses, [](const Status& s) {
        return s.state == ConnectionState::Disconnected; // a deliberate stop is not a failure
    }));
    source->stop(); // idempotent
}

} // namespace
} // namespace ieg::acquisition
