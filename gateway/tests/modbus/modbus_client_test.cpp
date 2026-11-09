// IModbusClient contract pieces that need no network: request validation, the result type, the
// error names and the scripted fake used by future poller tests.

#include "ieg/modbus/modbus_client.hpp"
#include "modbus/fake_modbus_client.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

namespace ieg::modbus {
namespace {

using config::ModbusTable;
using testing::FakeModbusClient;

std::optional<ModbusErrorKind> problem(ModbusTable table, std::uint16_t start, std::size_t quantity,
                                       bool bits)
{
    const auto error = validate_read(table, start, quantity, bits);
    return error ? std::optional{error->kind} : std::nullopt;
}

constexpr std::optional<ModbusErrorKind> kValid = std::nullopt;
constexpr std::optional<ModbusErrorKind> kInvalid = ModbusErrorKind::InvalidRequest;

ModbusError error_of(ModbusErrorKind kind)
{
    return ModbusError{
        .kind = kind, .exception_code = std::nullopt, .system_errno = std::nullopt, .detail = ""};
}

// ---- Request validation ----------------------------------------------------------------------

TEST(ModbusRequestValidation, AcceptsEveryTableWithinTheProtocolLimits)
{
    EXPECT_EQ(problem(ModbusTable::HoldingRegisters, 0, 1, false), kValid);
    EXPECT_EQ(problem(ModbusTable::InputRegisters, 0, 125, false), kValid);
    EXPECT_EQ(problem(ModbusTable::Coils, 0, 1, true), kValid);
    EXPECT_EQ(problem(ModbusTable::DiscreteInputs, 0, 2000, true), kValid);
}

TEST(ModbusRequestValidation, RejectsZeroAndOversizedQuantities)
{
    EXPECT_EQ(problem(ModbusTable::HoldingRegisters, 0, 0, false), kInvalid);
    EXPECT_EQ(problem(ModbusTable::HoldingRegisters, 0, 126, false), kInvalid);
    EXPECT_EQ(problem(ModbusTable::Coils, 0, 0, true), kInvalid);
    EXPECT_EQ(problem(ModbusTable::Coils, 0, 2001, true), kInvalid);
}

TEST(ModbusRequestValidation, AcceptsRequestsEndingAtTheLastAddress)
{
    EXPECT_EQ(problem(ModbusTable::InputRegisters, 65'535, 1, false), kValid);
    EXPECT_EQ(problem(ModbusTable::HoldingRegisters, 65'536 - 125, 125, false), kValid);
    EXPECT_EQ(problem(ModbusTable::Coils, 65'536 - 2000, 2000, true), kValid);
}

TEST(ModbusRequestValidation, RejectsRequestsBeyondTheLastAddress)
{
    EXPECT_EQ(problem(ModbusTable::HoldingRegisters, 65'535, 2, false), kInvalid);
    EXPECT_EQ(problem(ModbusTable::DiscreteInputs, 65'000, 537, true), kInvalid);
}

TEST(ModbusRequestValidation, RejectsTheWrongKindOfTable)
{
    EXPECT_EQ(problem(ModbusTable::Coils, 0, 1, false), kInvalid);
    EXPECT_EQ(problem(ModbusTable::InputRegisters, 0, 1, true), kInvalid);
}

TEST(ModbusRequestValidation, ExplainsTheProblem)
{
    const auto error = validate_read(ModbusTable::HoldingRegisters, 0, 126, false);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error.value_or(ModbusError{}).detail,
              "quantity 126 is outside 1..125 for one request");
}

// ---- Result type and names -------------------------------------------------------------------

TEST(ModbusResult, AccessorsEnforceTheirPreconditions)
{
    const auto success = ConnectResult::success(std::monostate{});
    const auto failure = ConnectResult::failure(error_of(ModbusErrorKind::Timeout));
    EXPECT_TRUE(success.ok());
    EXPECT_FALSE(failure.ok());
    EXPECT_EQ(failure.error().kind, ModbusErrorKind::Timeout);
    EXPECT_THROW(static_cast<void>(failure.value()), std::logic_error);
    EXPECT_THROW(static_cast<void>(success.error()), std::logic_error);
}

TEST(ModbusErrorKind, EveryKindHasADistinctName)
{
    std::set<std::string_view> names;
    for (const auto kind : {ModbusErrorKind::InvalidRequest, ModbusErrorKind::NotConnected,
                            ModbusErrorKind::ConnectionFailed, ModbusErrorKind::ConnectionLost,
                            ModbusErrorKind::Timeout, ModbusErrorKind::ModbusException,
                            ModbusErrorKind::ProtocolError}) {
        names.insert(to_string(kind));
    }
    EXPECT_EQ(names.size(), 7U);
    EXPECT_EQ(names.count("unknown"), 0U);
}

// ---- Fake client -----------------------------------------------------------------------------

TEST(FakeModbusClient, ReturnsScriptedRegistersAndRecordsRequests)
{
    FakeModbusClient client;
    ASSERT_TRUE(client.connect().ok());
    client.script_read(std::vector<std::uint16_t>{0x4049, 0x0FDB});
    std::array<std::uint16_t, 2> buffer{};
    const auto result = client.read_registers(ModbusTable::InputRegisters, 30, buffer);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().table, ModbusTable::InputRegisters);
    EXPECT_EQ(result.value().start, 30);
    EXPECT_EQ(buffer, (std::array<std::uint16_t, 2>{0x4049, 0x0FDB}));
    EXPECT_EQ(client.requests(),
              (std::vector<FakeModbusClient::Request>{{ModbusTable::InputRegisters, 30, 2}}));
}

TEST(FakeModbusClient, ReturnsScriptedBits)
{
    FakeModbusClient client;
    ASSERT_TRUE(client.connect().ok());
    client.script_read(std::vector<std::uint16_t>{1, 0, 1});
    std::array<std::uint8_t, 3> bits{};
    ASSERT_TRUE(client.read_bits(ModbusTable::Coils, 0, bits).ok());
    EXPECT_EQ(bits, (std::array<std::uint8_t, 3>{1, 0, 1}));
}

TEST(FakeModbusClient, FollowsTheConnectionRulesOfTheRealClient)
{
    FakeModbusClient client;
    std::array<std::uint16_t, 1> buffer{};
    EXPECT_EQ(client.read_registers(ModbusTable::HoldingRegisters, 0, buffer).error().kind,
              ModbusErrorKind::NotConnected);
    ASSERT_TRUE(client.connect().ok());
    client.script_read(error_of(ModbusErrorKind::ModbusException));
    client.script_read(error_of(ModbusErrorKind::Timeout));
    EXPECT_FALSE(client.read_registers(ModbusTable::HoldingRegisters, 0, buffer).ok());
    EXPECT_TRUE(client.connected()); // exceptions keep the connection
    EXPECT_FALSE(client.read_registers(ModbusTable::HoldingRegisters, 0, buffer).ok());
    EXPECT_FALSE(client.connected()); // a timeout closes it
}

TEST(FakeModbusClient, ScriptsConnectFailuresAndValidatesRequests)
{
    FakeModbusClient client;
    client.script_connect(error_of(ModbusErrorKind::ConnectionFailed));
    EXPECT_EQ(client.connect().error().kind, ModbusErrorKind::ConnectionFailed);
    EXPECT_FALSE(client.connected());
    ASSERT_TRUE(client.connect().ok());
    std::array<std::uint8_t, 1> bits{};
    EXPECT_EQ(client.read_bits(ModbusTable::HoldingRegisters, 0, bits).error().kind,
              ModbusErrorKind::InvalidRequest);
    EXPECT_TRUE(client.requests().empty());
    EXPECT_EQ(client.connect_calls(), 2);
}

} // namespace
} // namespace ieg::modbus
