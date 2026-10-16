#include "ieg/opcua/opcua_client.hpp"
#include "opcua/mock_opcua_client.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <type_traits>
#include <vector>

namespace ieg::opcua {
namespace {

using namespace std::chrono_literals;
using domain::DataQuality;
using ::testing::ElementsAre;
using ::testing::SaveArg;
using ::testing::StrictMock;

static_assert(std::is_abstract_v<IOpcUaClient>);
static_assert(std::has_virtual_destructor_v<IOpcUaClient>);
static_assert(!std::is_copy_constructible_v<IOpcUaClient>);
static_assert(!std::is_move_constructible_v<IOpcUaClient>);
static_assert(noexcept(std::declval<IOpcUaClient&>().stop()));

constexpr StatusCode Good = 0x00000000U;
constexpr StatusCode BadNodeIdUnknown = 0x80340000U;

constexpr domain::Timestamp kSourceTime{std::chrono::sys_days{std::chrono::year{2026} / 10 / 15} +
                                        10h};

// Stand-in for the gateway core: records everything delivered through the callbacks.
struct Recorder
{
    std::vector<DataChange> data;
    std::vector<ConnectionStatus> statuses;

    ClientCallbacks callbacks()
    {
        return ClientCallbacks{
            .on_data_change = [this](const DataChange& change) { data.push_back(change); },
            .on_status_change = [this](const ConnectionStatus& s) { statuses.push_back(s); },
        };
    }
};

// Plays the gateway core: owns the mocked client and the callbacks it registered.
struct Harness
{
    void start_client()
    {
        EXPECT_CALL(client, start(ElementsAre(MonitoredItem{1, "ns=2;s=Line01.Press01.Pressure"},
                                              MonitoredItem{2, "ns=2;s=Line01.Missing"}),
                                  ::testing::_))
            .WillOnce(SaveArg<1>(&captured));
        // The core only sees the abstract interface.
        IOpcUaClient& opcua = client;
        opcua.start({{1, "ns=2;s=Line01.Press01.Pressure"}, {2, "ns=2;s=Line01.Missing"}},
                    recorder.callbacks());
    }

    StrictMock<testing::MockOpcUaClient> client;
    ClientCallbacks captured;
    Recorder recorder;
};

TEST(OpcUaClientMock, CapturedCallbacksDeliverStatusAndData)
{
    Harness h;
    h.start_client();
    ASSERT_TRUE(h.captured.on_data_change);
    ASSERT_TRUE(h.captured.on_status_change);

    h.captured.on_status_change({ConnectionState::Connecting, ""});
    h.captured.on_status_change({ConnectionState::Connected, ""});
    h.captured.on_data_change(DataChange{
        .id = 1,
        .value = 180.25,
        .quality = quality_from_status_code(Good),
        .status_code = Good,
        .source_timestamp = kSourceTime,
    });

    EXPECT_THAT(h.recorder.statuses, ElementsAre(ConnectionStatus{ConnectionState::Connecting, ""},
                                                 ConnectionStatus{ConnectionState::Connected, ""}));
    ASSERT_EQ(h.recorder.data.size(), 1U);
    EXPECT_EQ(h.recorder.data[0].id, 1U);
    EXPECT_EQ(h.recorder.data[0].value, std::optional<domain::TelemetryValue>{180.25});
    EXPECT_EQ(h.recorder.data[0].quality, DataQuality::Good);
    EXPECT_EQ(h.recorder.data[0].source_timestamp, kSourceTime);
}

TEST(OpcUaClientMock, PerNodeFailureIsBadDataChangeWithoutValue)
{
    Harness h;
    h.start_client();
    h.captured.on_data_change(DataChange{
        .id = 2,
        .value = std::nullopt,
        .quality = quality_from_status_code(BadNodeIdUnknown),
        .status_code = BadNodeIdUnknown,
        .source_timestamp = std::nullopt,
    });

    ASSERT_EQ(h.recorder.data.size(), 1U);
    EXPECT_FALSE(h.recorder.data[0].value.has_value());
    EXPECT_EQ(h.recorder.data[0].quality, DataQuality::Bad);
    EXPECT_EQ(h.recorder.data[0].status_code, BadNodeIdUnknown);
    EXPECT_FALSE(h.recorder.data[0].source_timestamp.has_value());
}

TEST(OpcUaClientMock, LocalConversionFailureKeepsRawGoodStatusButIsBad)
{
    Harness h;
    h.start_client();
    // E.g. the server sent an array with StatusCode Good: the raw code is preserved and the
    // effective quality is Bad, so quality need not equal quality_from_status_code(status_code).
    h.captured.on_data_change(DataChange{
        .id = 1,
        .value = std::nullopt,
        .quality = DataQuality::Bad,
        .status_code = Good,
        .source_timestamp = kSourceTime,
    });

    ASSERT_EQ(h.recorder.data.size(), 1U);
    EXPECT_EQ(h.recorder.data[0].status_code, Good);
    EXPECT_EQ(h.recorder.data[0].quality, DataQuality::Bad);
    EXPECT_NE(h.recorder.data[0].quality, quality_from_status_code(h.recorder.data[0].status_code));
    EXPECT_EQ(h.recorder.data[0].source_timestamp, kSourceTime);
}

TEST(OpcUaClientMock, ConnectivityLossIsReportedAsStatusNotException)
{
    Harness h;
    h.start_client();
    h.captured.on_status_change({ConnectionState::Connected, ""});
    h.captured.on_status_change({ConnectionState::Disconnected, "BadConnectionClosed"});
    h.captured.on_status_change({ConnectionState::Connecting, ""});
    h.captured.on_status_change({ConnectionState::Connected, ""});

    ASSERT_EQ(h.recorder.statuses.size(), 4U);
    EXPECT_EQ(h.recorder.statuses[1].state, ConnectionState::Disconnected);
    EXPECT_EQ(h.recorder.statuses[1].detail, "BadConnectionClosed");
    EXPECT_EQ(h.recorder.statuses[3].state, ConnectionState::Connected);
}

TEST(OpcUaClientMock, StopIsExpectedThroughInterface)
{
    Harness h;
    h.start_client();
    EXPECT_CALL(h.client, stop()).Times(1);
    IOpcUaClient& opcua = h.client;
    opcua.stop();
}

} // namespace
} // namespace ieg::opcua
