#include "modbus/libmodbus_errors.hpp"

#include <gtest/gtest.h>
#include <modbus.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>

namespace ieg::modbus::detail {
namespace {

using namespace std::chrono_literals;

TEST(LibModbusErrors, MapsExceptionCodesWithoutReportingThemAsErrno)
{
    const auto illegal_address = classify_read_error(EMBXILADD);
    EXPECT_EQ(illegal_address.kind, ModbusErrorKind::ModbusException);
    EXPECT_EQ(illegal_address.exception_code, std::optional<std::uint8_t>{0x02});
    EXPECT_EQ(illegal_address.system_errno, std::nullopt);
    EXPECT_EQ(classify_read_error(EMBXILFUN).exception_code, std::optional<std::uint8_t>{0x01});
    EXPECT_EQ(classify_read_error(EMBXGTAR).exception_code, std::optional<std::uint8_t>{0x0B});
}

TEST(LibModbusErrors, MapsLibraryProtocolCodesToProtocolErrors)
{
    for (const int code : {EMBBADCRC, EMBBADDATA, EMBBADEXC, EMBUNKEXC, EMBMDATA, EMBBADSLAVE}) {
        const auto error = classify_read_error(code);
        EXPECT_EQ(error.kind, ModbusErrorKind::ProtocolError) << code;
        EXPECT_EQ(error.system_errno, std::nullopt) << code;
        EXPECT_EQ(error.exception_code, std::nullopt) << code;
    }
}

TEST(LibModbusErrors, MapsTimeoutsAndConnectionFailuresDuringReads)
{
    const auto timeout = classify_read_error(ETIMEDOUT);
    EXPECT_EQ(timeout.kind, ModbusErrorKind::Timeout);
    EXPECT_EQ(timeout.system_errno, std::optional<int>{ETIMEDOUT});
    for (const int code : {ECONNRESET, EPIPE, EBADF}) {
        const auto lost = classify_read_error(code);
        EXPECT_EQ(lost.kind, ModbusErrorKind::ConnectionLost) << code;
        EXPECT_EQ(lost.system_errno, std::optional<int>{code}) << code;
    }
    EXPECT_EQ(classify_read_error(0).kind, ModbusErrorKind::ProtocolError);
}

TEST(LibModbusErrors, MapsConnectFailuresIncludingTimeouts)
{
    const auto refused = classify_connect_error(ECONNREFUSED, 500ms);
    EXPECT_EQ(refused.kind, ModbusErrorKind::ConnectionFailed);
    EXPECT_EQ(refused.system_errno, std::optional<int>{ECONNREFUSED});
    for (const int code : {EINPROGRESS, ETIMEDOUT}) {
        const auto timed_out = classify_connect_error(code, 500ms);
        EXPECT_EQ(timed_out.kind, ModbusErrorKind::ConnectionFailed) << code;
        EXPECT_EQ(timed_out.detail, "connect timed out after 500 ms") << code;
    }
    EXPECT_EQ(classify_connect_error(0, 500ms).system_errno, std::nullopt);
}

TEST(LibModbusErrors, OnlyStreamDesynchronisingFailuresCloseTheConnection)
{
    EXPECT_TRUE(closes_connection(classify_read_error(ETIMEDOUT)));
    EXPECT_TRUE(closes_connection(classify_read_error(ECONNRESET)));
    EXPECT_TRUE(closes_connection(classify_read_error(EMBBADDATA)));
    EXPECT_FALSE(closes_connection(classify_read_error(EMBXILADD)));
    EXPECT_FALSE(closes_connection(classify_connect_error(ECONNREFUSED, 1ms)));
}

} // namespace
} // namespace ieg::modbus::detail
