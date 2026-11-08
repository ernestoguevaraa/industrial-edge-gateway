#include "ieg/acquisition/opcua_source.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ieg::acquisition {
namespace {

class FakeOpcUaClient final : public opcua::IOpcUaClient
{
public:
    void start(std::vector<opcua::MonitoredItem> requested,
               opcua::ClientCallbacks callbacks) override
    {
        items = std::move(requested);
        registered = std::move(callbacks);
        ++starts;
    }

    void stop() noexcept override { ++stops; }

    std::vector<opcua::MonitoredItem> items;
    opcua::ClientCallbacks registered;
    int starts{};
    int stops{};
};

TEST(OpcUaSource, PreservesTelemetryAndNativeStatusAsDiagnostic)
{
    auto client = std::make_unique<FakeOpcUaClient>();
    auto* fake = client.get();
    OpcUaSource source{"opcua-main", std::move(client)};

    std::vector<Update> updates;
    std::vector<Status> statuses;
    source.start({Item{.id = 7, .locator = "ns=2;s=Press.Pressure"}},
                 Callbacks{.on_update = [&](const Update& update) { updates.push_back(update); },
                           .on_status = [&](const Status& status) { statuses.push_back(status); }});
    ASSERT_EQ(fake->starts, 1);
    ASSERT_EQ(fake->items.size(), 1U);
    EXPECT_EQ(fake->items.front().id, 7U);
    EXPECT_EQ(fake->items.front().node_id, "ns=2;s=Press.Pressure");

    const domain::Timestamp timestamp{
        std::chrono::sys_days{std::chrono::year{2026} / 11 / 8}};
    fake->registered.on_status_change(
        opcua::ConnectionStatus{opcua::ConnectionState::Connected, "session active"});
    fake->registered.on_data_change(opcua::DataChange{
        .id = 7,
        .value = 145.5,
        .quality = domain::DataQuality::Uncertain,
        .status_code = 0x40000000U,
        .source_timestamp = timestamp,
    });
    ASSERT_EQ(statuses.size(), 1U);
    EXPECT_EQ(statuses[0].source_id, "opcua-main");
    EXPECT_EQ(statuses[0].state, ConnectionState::Connected);
    EXPECT_EQ(statuses[0].detail, "session active");
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].source_id, "opcua-main");
    EXPECT_EQ(updates[0].item_id, 7U);
    EXPECT_EQ(updates[0].value, std::optional<domain::TelemetryValue>{145.5});
    EXPECT_EQ(updates[0].quality, domain::DataQuality::Uncertain);
    EXPECT_EQ(updates[0].source_timestamp, timestamp);
    EXPECT_EQ(updates[0].diagnostic, "1073741824");
    source.stop();
    EXPECT_EQ(fake->stops, 1);
}

TEST(OpcUaSource, KeepsMissingValuesBadWithOriginalDiagnostic)
{
    auto client = std::make_unique<FakeOpcUaClient>();
    auto* fake = client.get();
    OpcUaSource source{"opcua-main", std::move(client)};
    std::vector<Update> updates;
    source.start({Item{1, "ns=2;s=Missing"}},
                 Callbacks{.on_update = [&](const Update& u) { updates.push_back(u); },
                           .on_status = [](const Status&) {}});
    fake->registered.on_data_change(opcua::DataChange{
        .id = 1,
        .value = std::nullopt,
        .quality = domain::DataQuality::Bad,
        .status_code = 0x80340000U,
        .source_timestamp = std::nullopt,
    });
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_FALSE(updates.front().value.has_value());
    EXPECT_EQ(updates.front().quality, domain::DataQuality::Bad);
    EXPECT_FALSE(updates.front().source_timestamp.has_value());
    EXPECT_EQ(updates.front().diagnostic, "2150891520");
}

TEST(OpcUaSource, RejectsInvalidRequestWithoutStartingClient)
{
    auto client = std::make_unique<FakeOpcUaClient>();
    auto* fake = client.get();
    OpcUaSource source{"opcua-main", std::move(client)};
    Callbacks callbacks{.on_update = [](const Update&) {},
                        .on_status = [](const Status&) {}};
    EXPECT_THROW(source.start({Item{1, "x"}, Item{1, "y"}}, callbacks),
                 std::invalid_argument);
    EXPECT_EQ(fake->starts, 0);
    source.start({Item{1, "ns=2;s=Good"}}, callbacks);
    EXPECT_THROW(source.start({Item{2, "ns=2;s=Other"}}, callbacks), std::logic_error);
    EXPECT_EQ(fake->starts, 1);
}

} // namespace
} // namespace ieg::acquisition
