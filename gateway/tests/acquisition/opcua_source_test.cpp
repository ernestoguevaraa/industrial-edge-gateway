#include "ieg/acquisition/opcua_source.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ieg::acquisition {
namespace {

constexpr const char* kSourceId = "opcua-main";
constexpr opcua::StatusCode kUncertainInitialValue = 0x40920000U;
constexpr opcua::StatusCode kBadNodeIdUnknown = 0x80340000U;

// Records what OpcUaSource does to the wrapped client; can make start() throw and can play the
// client worker thread by invoking the registered callbacks.
class FakeOpcUaClient final : public opcua::IOpcUaClient
{
public:
    void start(std::vector<opcua::MonitoredItem> requested,
               opcua::ClientCallbacks callbacks) override
    {
        ++starts;
        if (start_error) {
            std::rethrow_exception(start_error);
        }
        items = std::move(requested);
        registered = std::move(callbacks);
    }

    void stop() noexcept override { ++stops; }

    std::exception_ptr start_error;
    std::vector<opcua::MonitoredItem> items;
    opcua::ClientCallbacks registered;
    int starts{};
    int stops{};
};

// Counts stop() calls in caller-owned storage, so they stay observable after destruction.
class StopCounter final : public opcua::IOpcUaClient
{
public:
    explicit StopCounter(int& stops) : stops_(&stops) {}
    void start(std::vector<opcua::MonitoredItem> /*items*/,
               opcua::ClientCallbacks /*callbacks*/) override
    {}
    void stop() noexcept override { ++*stops_; }

private:
    int* stops_;
};

// Owns an OpcUaSource around a FakeOpcUaClient and records the forwarded callbacks.
class SourceHarness
{
public:
    SourceHarness()
    {
        auto client = std::make_unique<FakeOpcUaClient>();
        fake_ = client.get();
        source_ = std::make_unique<OpcUaSource>(kSourceId, std::move(client));
    }

    [[nodiscard]] Callbacks recording_callbacks()
    {
        return Callbacks{.on_update = [this](const Update& u) { updates_.push_back(u); },
                         .on_status = [this](const Status& s) { statuses_.push_back(s); }};
    }

    void start(std::vector<Item> items) { source_->start(std::move(items), recording_callbacks()); }

    OpcUaSource& source() { return *source_; }
    FakeOpcUaClient& client() { return *fake_; }
    [[nodiscard]] const std::vector<Update>& updates() const { return updates_; }
    [[nodiscard]] const std::vector<Status>& statuses() const { return statuses_; }

private:
    std::vector<Update> updates_;
    std::vector<Status> statuses_;
    FakeOpcUaClient* fake_{nullptr};
    std::unique_ptr<OpcUaSource> source_; // destroyed first: stop() never sees a dead client
};

Callbacks no_op_callbacks()
{
    return Callbacks{.on_update = [](const Update&) {}, .on_status = [](const Status&) {}};
}

constexpr domain::Timestamp kSourceTime{std::chrono::sys_days{std::chrono::year{2026} / 11 / 8}};

// ---- Construction and identity ---------------------------------------------------------------

TEST(OpcUaSource, ExposesItsSourceId)
{
    SourceHarness harness;
    EXPECT_EQ(harness.source().id(), kSourceId);
}

TEST(OpcUaSource, RejectsEmptySourceIdOrMissingClient)
{
    EXPECT_THROW(OpcUaSource("", std::make_unique<FakeOpcUaClient>()), std::invalid_argument);
    EXPECT_THROW(OpcUaSource(kSourceId, nullptr), std::invalid_argument);
}

// ---- Mapping ---------------------------------------------------------------------------------

TEST(OpcUaSource, ForwardsItemsAsMonitoredItemsInOrder)
{
    SourceHarness harness;
    harness.start({Item{.id = 7, .locator = "ns=2;s=Press.Pressure"},
                   Item{.id = 3, .locator = "ns=2;s=Press.State"}});
    const std::vector<opcua::MonitoredItem> expected{{7, "ns=2;s=Press.Pressure"},
                                                     {3, "ns=2;s=Press.State"}};
    EXPECT_EQ(harness.client().items, expected);
}

TEST(OpcUaSource, PreservesValueQualityTimestampAndRawStatusCode)
{
    SourceHarness harness;
    harness.start({Item{.id = 7, .locator = "ns=2;s=Press.Pressure"}});
    harness.client().registered.on_data_change(opcua::DataChange{
        .id = 7,
        .value = 145.5,
        .quality = domain::DataQuality::Uncertain,
        .status_code = kUncertainInitialValue,
        .source_timestamp = kSourceTime,
    });
    const std::vector<Update> expected{Update{.source_id = kSourceId,
                                              .item_id = 7,
                                              .value = 145.5,
                                              .quality = domain::DataQuality::Uncertain,
                                              .source_timestamp = kSourceTime,
                                              .native_status = kUncertainInitialValue,
                                              .diagnostic = {}}};
    EXPECT_EQ(harness.updates(), expected);
}

TEST(OpcUaSource, KeepsMissingValueBadWithOriginalStatusCodeAndNoTimestamp)
{
    SourceHarness harness;
    harness.start({Item{.id = 1, .locator = "ns=2;s=Missing"}});
    harness.client().registered.on_data_change(opcua::DataChange{
        .id = 1,
        .value = std::nullopt,
        .quality = domain::DataQuality::Bad,
        .status_code = kBadNodeIdUnknown,
        .source_timestamp = std::nullopt,
    });
    const std::vector<Update> expected{Update{.source_id = kSourceId,
                                              .item_id = 1,
                                              .value = std::nullopt,
                                              .quality = domain::DataQuality::Bad,
                                              .source_timestamp = std::nullopt,
                                              .native_status = kBadNodeIdUnknown,
                                              .diagnostic = {}}};
    EXPECT_EQ(harness.updates(), expected);
}

TEST(OpcUaSource, KeepsGoodStatusCodeWhenTheClientReportsALocalConversionFailure)
{
    // The client contract: a local conversion failure keeps the server's Good StatusCode but
    // reports quality Bad without a value. The bridge must not "repair" either field.
    SourceHarness harness;
    harness.start({Item{.id = 2, .locator = "ns=2;s=Array"}});
    harness.client().registered.on_data_change(opcua::DataChange{
        .id = 2,
        .value = std::nullopt,
        .quality = domain::DataQuality::Bad,
        .status_code = 0U,
        .source_timestamp = kSourceTime,
    });
    ASSERT_EQ(harness.updates().size(), 1U);
    EXPECT_EQ(harness.updates().front().quality, domain::DataQuality::Bad);
    EXPECT_EQ(harness.updates().front().native_status, std::optional<NativeStatusCode>{0U});
}

TEST(OpcUaSource, MapsEveryConnectionStateWithDetail)
{
    SourceHarness harness;
    harness.start({Item{.id = 1, .locator = "ns=2;s=X"}});
    for (const auto state : {opcua::ConnectionState::Connecting, opcua::ConnectionState::Connected,
                             opcua::ConnectionState::Disconnected}) {
        harness.client().registered.on_status_change(opcua::ConnectionStatus{state, "detail"});
    }
    const std::vector<Status> expected{
        {.source_id = kSourceId, .state = ConnectionState::Connecting, .detail = "detail"},
        {.source_id = kSourceId, .state = ConnectionState::Connected, .detail = "detail"},
        {.source_id = kSourceId, .state = ConnectionState::Disconnected, .detail = "detail"}};
    EXPECT_EQ(harness.statuses(), expected);
}

// ---- Request validation ----------------------------------------------------------------------

TEST(OpcUaSource, RejectsInvalidRequestsWithoutStartingTheClient)
{
    SourceHarness harness;
    const auto callbacks = no_op_callbacks();
    EXPECT_THROW(harness.source().start({}, callbacks), std::invalid_argument);
    EXPECT_THROW(harness.source().start({Item{1, "x"}, Item{1, "y"}}, callbacks),
                 std::invalid_argument);
    EXPECT_THROW(harness.source().start({Item{1, ""}}, callbacks), std::invalid_argument);
    EXPECT_THROW(harness.source().start({Item{1, "x"}}, Callbacks{}), std::invalid_argument);
    EXPECT_EQ(harness.client().starts, 0);
}

TEST(OpcUaSource, InvalidRequestDoesNotConsumeTheSingleStart)
{
    SourceHarness harness;
    EXPECT_THROW(harness.source().start({}, no_op_callbacks()), std::invalid_argument);
    harness.start({Item{1, "ns=2;s=Good"}});
    EXPECT_EQ(harness.client().starts, 1);
}

// ---- Lifecycle -------------------------------------------------------------------------------

TEST(OpcUaSource, StartsAtMostOnce)
{
    SourceHarness harness;
    harness.start({Item{1, "ns=2;s=Good"}});
    EXPECT_THROW(harness.start({Item{2, "ns=2;s=Other"}}), std::logic_error);
    EXPECT_EQ(harness.client().starts, 1);
}

TEST(OpcUaSource, CannotStartAfterStop)
{
    SourceHarness harness;
    harness.source().stop();
    EXPECT_THROW(harness.start({Item{1, "ns=2;s=Good"}}), std::logic_error);
    EXPECT_EQ(harness.client().starts, 0);
}

TEST(OpcUaSource, StopIsIdempotentSafeBeforeStartAndForwardedToTheClient)
{
    SourceHarness harness;
    harness.source().stop();
    EXPECT_EQ(harness.client().stops, 1);
    harness.source().stop();
    EXPECT_EQ(harness.client().stops, 2); // the client's stop() is itself idempotent
}

TEST(OpcUaSource, ClientRejectionLeavesTheSourceStartable)
{
    // E.g. the client rejects an unparsable NodeId: nothing was started.
    SourceHarness harness;
    harness.client().start_error =
        std::make_exception_ptr(std::invalid_argument("unparsable NodeId"));
    EXPECT_THROW(harness.start({Item{1, "not-a-node-id"}}), std::invalid_argument);
    EXPECT_EQ(harness.client().stops, 0);

    harness.client().start_error = nullptr;
    harness.start({Item{1, "ns=2;s=Good"}});
    EXPECT_EQ(harness.client().starts, 2);
}

TEST(OpcUaSource, UnexpectedClientStartFailureStopsTheClientAndConsumesTheStart)
{
    SourceHarness harness;
    harness.client().start_error = std::make_exception_ptr(std::runtime_error("no thread"));
    EXPECT_THROW(harness.start({Item{1, "ns=2;s=Good"}}), std::runtime_error);
    EXPECT_EQ(harness.client().stops, 1); // partially started client released at once

    harness.client().start_error = nullptr;
    EXPECT_THROW(harness.start({Item{1, "ns=2;s=Good"}}), std::logic_error);
    EXPECT_EQ(harness.client().starts, 1);
}

TEST(OpcUaSource, DestructorStopsTheClient)
{
    // The client is owned (and destroyed) by the source, so count stops outside of it.
    int stops = 0;
    {
        const OpcUaSource source{kSourceId, std::make_unique<StopCounter>(stops)};
    }
    EXPECT_EQ(stops, 1);
}

} // namespace
} // namespace ieg::acquisition
