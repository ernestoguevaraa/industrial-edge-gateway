// LibModbusClient against the deterministic in-process Modbus TCP server (127.0.0.1, ephemeral
// port). No external process or infrastructure is involved.

#include "ieg/modbus/libmodbus_client.hpp"
#include "modbus/modbus_test_server.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

namespace ieg::modbus {
namespace {

using namespace std::chrono_literals;
using config::ModbusTable;
using testing::ModbusTestServer;
using Mode = ModbusTestServer::Mode;
using Clock = std::chrono::steady_clock;

constexpr std::uint8_t kUnitId = 17;
constexpr auto kTimeout = 200ms;

LibModbusClientOptions options_for(std::uint16_t port)
{
    return LibModbusClientOptions{
        .host = "127.0.0.1", .port = port, .unit_id = kUnitId, .timeout = kTimeout};
}

// A loopback port with nothing listening (bound, then released).
std::uint16_t unused_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX sockaddr API.
    auto* generic = reinterpret_cast<sockaddr*>(&address);
    socklen_t length = sizeof(address);
    const bool bound = fd >= 0 && ::bind(fd, generic, sizeof(address)) == 0 &&
                       ::getsockname(fd, generic, &length) == 0;
    if (fd >= 0) {
        ::close(fd);
    }
    if (!bound) {
        throw std::runtime_error("cannot reserve a loopback port");
    }
    return ntohs(address.sin_port);
}

// A ModbusException carrying `code`, never reported as a system errno.
::testing::AssertionResult is_exception(const ModbusError& error, std::uint8_t code)
{
    if (error.kind != ModbusErrorKind::ModbusException ||
        error.exception_code != std::optional<std::uint8_t>{code} ||
        error.system_errno.has_value()) {
        return ::testing::AssertionFailure()
               << "expected Modbus exception " << int{code} << ", got " << to_string(error.kind)
               << ": " << error.detail;
    }
    return ::testing::AssertionSuccess();
}

// The failure a read reports when the server has already closed the connection: the request may
// still be sent, then the closed stream is detected (ConnectionLost).
::testing::AssertionResult is_connection_loss(std::optional<ModbusErrorKind> failure)
{
    if (failure != ModbusErrorKind::ConnectionLost) {
        return ::testing::AssertionFailure()
               << "expected connection_lost, got " << (failure ? to_string(*failure) : "ok");
    }
    return ::testing::AssertionSuccess();
}

class LibModbusClientTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        client_ = std::make_unique<LibModbusClient>(options_for(server_.port()));
    }

    void connect()
    {
        const auto result = client_->connect();
        ASSERT_TRUE(result.ok()) << result.error().detail;
    }

    // Reads `count` holding registers at `start`; the error kind on failure.
    [[nodiscard]] std::optional<ModbusErrorKind> read_failure(std::uint16_t start = 0,
                                                              std::size_t count = 1)
    {
        buffer_.assign(count, 0);
        const auto result = client_->read_registers(ModbusTable::HoldingRegisters, start, buffer_);
        return result.ok() ? std::nullopt : std::optional{result.error().kind};
    }

    [[nodiscard]] ModbusError read_error()
    {
        buffer_.assign(1, 0);
        return client_->read_registers(ModbusTable::HoldingRegisters, 0, buffer_).error();
    }

    ModbusTestServer& server() { return server_; }
    LibModbusClient& client() { return *client_; }
    void destroy_client() { client_.reset(); }
    [[nodiscard]] const std::vector<std::uint16_t>& buffer() const { return buffer_; }

private:
    ModbusTestServer server_;
    std::unique_ptr<LibModbusClient> client_;
    std::vector<std::uint16_t> buffer_;
};

// ---- Construction ------------------------------------------------------------------------------

TEST(LibModbusClientOptions, RejectsInvalidOptions)
{
    auto valid = options_for(1502);
    EXPECT_NO_THROW(LibModbusClient{valid});
    for (const auto& change : std::vector<void (*)(LibModbusClientOptions&)>{
             [](LibModbusClientOptions& o) { o.host.clear(); },
             [](LibModbusClientOptions& o) { o.port = 0; },
             [](LibModbusClientOptions& o) { o.unit_id = 0; },
             [](LibModbusClientOptions& o) { o.timeout = 0ms; },
             [](LibModbusClientOptions& o) { o.timeout = 60'001ms; }}) {
        auto options = valid;
        change(options);
        EXPECT_THROW(LibModbusClient{options}, std::invalid_argument);
    }
}

TEST(LibModbusClientOptions, MapsTheDeviceConfiguration)
{
    config::ModbusDeviceConfig device;
    device.source_id = "press-plc";
    device.host = "modbus-simulator";
    device.port = 1502;
    device.unit_id = 7;
    device.timeout = 250ms;
    const auto options = client_options_from(device);
    EXPECT_EQ(options.host, "modbus-simulator");
    EXPECT_EQ(options.port, 1502);
    EXPECT_EQ(options.unit_id, 7);
    EXPECT_EQ(options.timeout, 250ms);
}

TEST(LibModbusClientOptions, ConstructionDoesNoNetworkActivity)
{
    const ModbusTestServer server;
    const LibModbusClient client{options_for(server.port())};
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(server.accepted(), 0);
    EXPECT_FALSE(client.connected());
}

// ---- Valid reads -------------------------------------------------------------------------------

TEST_F(LibModbusClientTest, ReadsHoldingRegistersWithTheConfiguredUnitId)
{
    server().set_holding(100, 0x1234);
    server().set_holding(101, 0xFFFF);
    server().set_holding(102, 0x0000);
    connect();
    std::array<std::uint16_t, 3> out{};
    const auto result = client().read_registers(ModbusTable::HoldingRegisters, 100, out);
    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value().table, ModbusTable::HoldingRegisters);
    EXPECT_EQ(result.value().start, 100);
    EXPECT_EQ(result.value().values.data(), out.data());
    EXPECT_EQ(out, (std::array<std::uint16_t, 3>{0x1234, 0xFFFF, 0x0000}));
    EXPECT_EQ(server().last_unit_id(), kUnitId);
    EXPECT_EQ(server().last_function(), 0x03);
}

TEST_F(LibModbusClientTest, ReadsInputRegistersWithFunctionCode4)
{
    server().set_input(7, 0xBEEF);
    connect();
    std::array<std::uint16_t, 1> out{};
    const auto result = client().read_registers(ModbusTable::InputRegisters, 7, out);
    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value().table, ModbusTable::InputRegisters);
    EXPECT_EQ(out[0], 0xBEEF);
    EXPECT_EQ(server().last_function(), 0x04);
}

TEST_F(LibModbusClientTest, ReadsCoilsAcrossByteBoundaries)
{
    for (const auto address : std::array<std::uint16_t, 4>{10, 12, 17, 19}) {
        server().set_coil(address, true);
    }
    connect();
    std::array<std::uint8_t, 10> out{};
    const auto result = client().read_bits(ModbusTable::Coils, 10, out);
    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(result.value().table, ModbusTable::Coils);
    EXPECT_EQ(result.value().start, 10);
    EXPECT_EQ(out, (std::array<std::uint8_t, 10>{1, 0, 1, 0, 0, 0, 0, 1, 0, 1}));
    EXPECT_EQ(server().last_function(), 0x01);
}

TEST_F(LibModbusClientTest, ReadsDiscreteInputsWithFunctionCode2)
{
    server().set_discrete(3, true);
    connect();
    std::array<std::uint8_t, 2> out{};
    const auto result = client().read_bits(ModbusTable::DiscreteInputs, 2, out);
    ASSERT_TRUE(result.ok()) << result.error().detail;
    EXPECT_EQ(out, (std::array<std::uint8_t, 2>{0, 1}));
    EXPECT_EQ(server().last_function(), 0x02);
}

TEST_F(LibModbusClientTest, ReadsTheMaximumQuantitiesAtTheEndOfTheAddressSpace)
{
    server().set_holding(65'535, 0xCAFE);
    server().set_coil(65'535, true);
    connect();
    std::vector<std::uint16_t> registers(kMaxReadRegisters);
    ASSERT_TRUE(client()
                    .read_registers(ModbusTable::HoldingRegisters,
                                    static_cast<std::uint16_t>(kAddressSpace - kMaxReadRegisters),
                                    registers)
                    .ok());
    EXPECT_EQ(registers.back(), 0xCAFE);
    std::vector<std::uint8_t> bits(kMaxReadBits);
    ASSERT_TRUE(client()
                    .read_bits(ModbusTable::Coils,
                               static_cast<std::uint16_t>(kAddressSpace - kMaxReadBits), bits)
                    .ok());
    EXPECT_EQ(bits.back(), 1);
}

TEST_F(LibModbusClientTest, ReturnsBlocksTheDecoderAccepts)
{
    server().set_holding(20, 0x4049);
    server().set_holding(21, 0x0FDB);
    connect();
    std::array<std::uint16_t, 2> out{};
    const auto block = client().read_registers(ModbusTable::HoldingRegisters, 20, out);
    ASSERT_TRUE(block.ok());
    config::ModbusPointConfig point;
    point.table = ModbusTable::HoldingRegisters;
    point.offset = 20;
    point.type = config::ModbusDataType::Float32;
    point.word_order = config::ModbusWordOrder::HighWordFirst;
    const auto decoded = decode(point, block.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_FLOAT_EQ(static_cast<float>(std::get<double>(decoded.value())), 3.14159274F);
}

// ---- Invalid requests ---------------------------------------------------------------------------

TEST_F(LibModbusClientTest, RejectsInvalidRequestsBeforeAnyIo)
{
    connect();
    EXPECT_EQ(read_failure(0, 0), ModbusErrorKind::InvalidRequest);
    EXPECT_EQ(read_failure(0, 126), ModbusErrorKind::InvalidRequest);
    EXPECT_EQ(read_failure(65'535, 2), ModbusErrorKind::InvalidRequest);
    std::vector<std::uint8_t> bits(kMaxReadBits + 1);
    EXPECT_EQ(client().read_bits(ModbusTable::Coils, 0, bits).error().kind,
              ModbusErrorKind::InvalidRequest);
    std::array<std::uint8_t, 1> one_bit{};
    EXPECT_EQ(client().read_bits(ModbusTable::HoldingRegisters, 0, one_bit).error().kind,
              ModbusErrorKind::InvalidRequest);
    std::array<std::uint16_t, 1> one_register{};
    EXPECT_EQ(client().read_registers(ModbusTable::Coils, 0, one_register).error().kind,
              ModbusErrorKind::InvalidRequest);
    EXPECT_EQ(server().requests(), 0);
    EXPECT_TRUE(client().connected()); // a rejected request does not touch the connection
}

// ---- Unavailable server -------------------------------------------------------------------------

TEST(LibModbusClientUnavailable, ReportsConnectionFailedWithTheSystemErrno)
{
    LibModbusClient client{options_for(unused_port())};
    const auto result = client.connect();
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().kind, ModbusErrorKind::ConnectionFailed);
    EXPECT_EQ(result.error().system_errno, std::optional<int>{ECONNREFUSED});
    EXPECT_EQ(result.error().exception_code, std::nullopt);
    EXPECT_FALSE(client.connected());
}

TEST(LibModbusClientUnavailable, NeverReadsWithoutAnExplicitConnect)
{
    const ModbusTestServer server;
    LibModbusClient client{options_for(server.port())};
    std::array<std::uint16_t, 1> out{};
    EXPECT_EQ(client.read_registers(ModbusTable::HoldingRegisters, 0, out).error().kind,
              ModbusErrorKind::NotConnected);
    EXPECT_EQ(server.accepted(), 0);
}

// ---- Timeouts -----------------------------------------------------------------------------------

TEST_F(LibModbusClientTest, TimesOutWithinTheConfiguredTimeoutAndClosesTheConnection)
{
    connect();
    server().set_mode(Mode::Silent);
    const auto before = Clock::now();
    const auto error = read_error();
    const auto took = Clock::now() - before;
    EXPECT_EQ(error.kind, ModbusErrorKind::Timeout);
    EXPECT_EQ(error.system_errno, std::optional<int>{ETIMEDOUT});
    EXPECT_GE(took, kTimeout - 20ms);
    EXPECT_LT(took, kTimeout + 800ms);
    EXPECT_FALSE(client().connected());
    EXPECT_EQ(read_failure(), ModbusErrorKind::NotConnected); // no implicit reconnect
}

TEST_F(LibModbusClientTest, ATricklingResponseCannotExtendTheTimeout)
{
    // The server sends a valid 11-byte response one byte every 50 ms (~550 ms in total). With
    // the byte timeout disabled, the whole response must arrive within the 200 ms timeout.
    connect();
    server().set_mode(Mode::Trickle);
    const auto before = Clock::now();
    EXPECT_EQ(read_failure(0, 1), ModbusErrorKind::Timeout);
    const auto took = Clock::now() - before;
    EXPECT_GE(took, kTimeout - 20ms);
    EXPECT_LT(took, kTimeout + 250ms);
    EXPECT_FALSE(client().connected());
}

TEST_F(LibModbusClientTest, ReconnectsExplicitlyAfterATimeout)
{
    server().set_holding(0, 42);
    connect();
    server().set_mode(Mode::Silent);
    EXPECT_EQ(read_failure(), ModbusErrorKind::Timeout);
    server().set_mode(Mode::Normal);
    connect();
    EXPECT_EQ(read_failure(), std::nullopt);
    EXPECT_EQ(buffer().at(0), 42);
    EXPECT_TRUE(ModbusTestServer::wait_until([&] { return server().accepted() == 2; }));
}

// ---- Modbus exceptions and protocol errors ------------------------------------------------------

TEST_F(LibModbusClientTest, ReportsModbusExceptionCodesAndKeepsTheConnection)
{
    connect();
    for (const auto code : std::array<std::uint8_t, 6>{0x01, 0x02, 0x03, 0x04, 0x06, 0x0B}) {
        server().set_mode(Mode::Exception, code);
        EXPECT_TRUE(is_exception(read_error(), code));
        EXPECT_TRUE(client().connected());
    }
    server().set_mode(Mode::Normal);
    EXPECT_EQ(read_failure(), std::nullopt);
    EXPECT_EQ(server().accepted(), 1); // the same connection served every request
}

TEST_F(LibModbusClientTest, ReportsAnUnknownExceptionCodeAsProtocolError)
{
    // libmodbus 3.1.10 does not preserve exception codes >= 12 (EMBBADEXC).
    connect();
    server().set_mode(Mode::Exception, 0x30);
    const auto error = read_error();
    EXPECT_EQ(error.kind, ModbusErrorKind::ProtocolError);
    EXPECT_EQ(error.exception_code, std::nullopt);
    EXPECT_FALSE(client().connected());
}

TEST_F(LibModbusClientTest, ReportsMismatchedTransactionIdAsProtocolError)
{
    connect();
    server().set_mode(Mode::WrongTransactionId);
    EXPECT_EQ(read_failure(), ModbusErrorKind::ProtocolError);
    EXPECT_FALSE(client().connected());
}

TEST_F(LibModbusClientTest, ReportsTruncatedResponseAsProtocolError)
{
    connect();
    server().set_mode(Mode::TruncatedResponse);
    EXPECT_EQ(read_failure(0, 3), ModbusErrorKind::ProtocolError);
    EXPECT_FALSE(client().connected());
}

// ---- Connection loss, reconnect, close and destruction ------------------------------------------

TEST_F(LibModbusClientTest, ReportsConnectionLostWhenTheServerClosesDuringARequest)
{
    connect();
    server().set_mode(Mode::CloseOnRequest);
    const auto error = read_error();
    EXPECT_EQ(error.kind, ModbusErrorKind::ConnectionLost);
    EXPECT_TRUE(error.system_errno.has_value());
    EXPECT_FALSE(client().connected());
}

TEST_F(LibModbusClientTest, ReconnectsExplicitlyAfterTheServerDroppedTheConnection)
{
    server().set_holding(5, 99);
    connect();
    ASSERT_TRUE(ModbusTestServer::wait_until([&] { return server().accepted() == 1; }));
    server().drop_connection();
    ASSERT_TRUE(ModbusTestServer::wait_until([&] { return server().dropped() == 1; }));
    EXPECT_TRUE(is_connection_loss(read_failure(5)));
    EXPECT_FALSE(client().connected());
    connect();
    EXPECT_EQ(read_failure(5), std::nullopt);
    EXPECT_EQ(buffer().at(0), 99);
}

TEST_F(LibModbusClientTest, ConnectWhileConnectedReplacesTheConnection)
{
    connect();
    connect();
    EXPECT_TRUE(ModbusTestServer::wait_until([&] { return server().accepted() == 2; }));
    EXPECT_TRUE(ModbusTestServer::wait_until([&] { return server().closed_by_peer() >= 1; }));
    EXPECT_EQ(read_failure(), std::nullopt);
}

TEST_F(LibModbusClientTest, CloseIsIdempotentAndClosesTheSocket)
{
    client().close(); // before connect
    connect();
    client().close();
    client().close();
    EXPECT_FALSE(client().connected());
    EXPECT_TRUE(ModbusTestServer::wait_until([&] { return server().closed_by_peer() == 1; }));
    EXPECT_EQ(read_failure(), ModbusErrorKind::NotConnected);
}

TEST_F(LibModbusClientTest, DestructionClosesTheConnection)
{
    connect();
    destroy_client();
    EXPECT_TRUE(ModbusTestServer::wait_until([&] { return server().closed_by_peer() == 1; }));
}

TEST(LibModbusClientLifetime, DestructionWithoutConnectIsSafe)
{
    const ModbusTestServer server;
    {
        const LibModbusClient client{options_for(server.port())};
    }
    EXPECT_EQ(server.accepted(), 0);
}

} // namespace
} // namespace ieg::modbus
