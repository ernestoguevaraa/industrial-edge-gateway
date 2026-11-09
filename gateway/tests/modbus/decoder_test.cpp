#include "ieg/modbus/decoder.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ieg::modbus {

// Readable gtest output for DecodeResult (found through ADL).
void PrintTo(const DecodeResult& result, std::ostream* os)
{
    if (result.ok()) {
        *os << "ok(" << ::testing::PrintToString(result.value()) << ")";
    } else {
        *os << "error(" << to_string(result.error()) << ")";
    }
}

namespace {

using config::ModbusDataType;
using config::ModbusPointConfig;
using config::ModbusTable;
using config::ModbusWordOrder;

constexpr auto kHigh = ModbusWordOrder::HighWordFirst;
constexpr auto kLow = ModbusWordOrder::LowWordFirst;
// float32 pi (bit pattern 0x40490FDB) widened exactly to double: 3.1415927410125732421875.
constexpr double kFloatPi = static_cast<double>(std::numbers::pi_v<float>);

ModbusPointConfig point(ModbusDataType type, std::optional<ModbusWordOrder> order = std::nullopt,
                        std::optional<double> scale = std::nullopt, std::uint16_t offset = 0,
                        ModbusTable table = ModbusTable::HoldingRegisters)
{
    ModbusPointConfig result;
    result.asset = "site01.line01.comp01";
    result.tag = "value";
    result.table = table;
    result.offset = offset;
    result.type = type;
    result.word_order = order;
    result.scale = scale;
    return result;
}

ModbusPointConfig bit_point(ModbusTable table, std::uint16_t offset = 0)
{
    return point(ModbusDataType::Bool, std::nullopt, std::nullopt, offset, table);
}

// Decodes from holding registers starting at `start`.
DecodeResult registers(const ModbusPointConfig& p, const std::vector<std::uint16_t>& values,
                       std::uint16_t start = 0, ModbusTable table = ModbusTable::HoldingRegisters)
{
    return decode(p, RegisterBlock{.table = table, .start = start, .values = values});
}

DecodeResult bits(const ModbusPointConfig& p, const std::vector<std::uint8_t>& values,
                  std::uint16_t start = 0)
{
    return decode(p, BitBlock{.table = p.table, .start = start, .values = values});
}

DecodeResult unsigned_value(std::uint64_t value)
{
    return DecodeResult::success(value);
}

DecodeResult signed_value(std::int64_t value)
{
    return DecodeResult::success(value);
}

DecodeResult double_value(double value)
{
    return DecodeResult::success(value);
}

DecodeResult failed(DecodeError error)
{
    return DecodeResult::failure(error);
}

// float32 registers (high word first) for a given bit pattern.
std::vector<std::uint16_t> words_of(std::uint32_t bits_pattern)
{
    return {static_cast<std::uint16_t>(bits_pattern >> 16U),
            static_cast<std::uint16_t>(bits_pattern & 0xFFFFU)};
}

double scaled(const DecodeResult& result)
{
    return std::get<double>(result.value());
}

// ---- Bits ------------------------------------------------------------------------------------

TEST(ModbusDecoder, DecodesCoilsAndDiscreteInputsAsBool)
{
    const std::vector<std::uint8_t> values{0, 1, 0, 1};
    for (const auto table : {ModbusTable::Coils, ModbusTable::DiscreteInputs}) {
        EXPECT_EQ(bits(bit_point(table, 0), values), DecodeResult::success(false));
        EXPECT_EQ(bits(bit_point(table, 3), values), DecodeResult::success(true));
    }
}

TEST(ModbusDecoder, AnyNonZeroBitElementIsTrue)
{
    EXPECT_EQ(bits(bit_point(ModbusTable::Coils), {0xFF}), DecodeResult::success(true));
}

TEST(ModbusDecoder, BitOffsetIsRelativeToTheBlockStart)
{
    EXPECT_EQ(bits(bit_point(ModbusTable::Coils, 102), {0, 0, 1}, 100),
              DecodeResult::success(true));
}

TEST(ModbusDecoder, RejectsBitsOutsideTheBlock)
{
    const auto coil = bit_point(ModbusTable::Coils, 5);
    EXPECT_EQ(bits(coil, {1, 1, 1, 1, 1}), failed(DecodeError::InsufficientData)); // past the end
    EXPECT_EQ(bits(coil, {1}, 6), failed(DecodeError::InsufficientData)); // before the start
    EXPECT_EQ(bits(coil, {}, 5), failed(DecodeError::InsufficientData));  // empty block
}

TEST(ModbusDecoder, RejectsBitBlockFromAnotherTableOrForARegisterType)
{
    const auto coil = bit_point(ModbusTable::Coils);
    const std::vector<std::uint8_t> values{1};
    EXPECT_EQ(
        decode(coil, BitBlock{.table = ModbusTable::DiscreteInputs, .start = 0, .values = values}),
        failed(DecodeError::TableMismatch));
    const auto register_type =
        point(ModbusDataType::UInt16, std::nullopt, std::nullopt, 0, ModbusTable::Coils);
    EXPECT_EQ(bits(register_type, values), failed(DecodeError::TypeTableMismatch));
}

TEST(ModbusDecoder, RejectsScaleOrWordOrderOnBool)
{
    auto with_scale = bit_point(ModbusTable::Coils);
    with_scale.scale = 2.0;
    EXPECT_EQ(bits(with_scale, {1}), failed(DecodeError::InvalidScale));
    auto with_order = bit_point(ModbusTable::Coils);
    with_order.word_order = kHigh;
    EXPECT_EQ(bits(with_order, {1}), failed(DecodeError::InvalidWordOrder));
}

// ---- 16-bit registers ------------------------------------------------------------------------

TEST(ModbusDecoder, DecodesUInt16AcrossItsRange)
{
    const auto p = point(ModbusDataType::UInt16);
    EXPECT_EQ(registers(p, {0x0000}), unsigned_value(0));
    EXPECT_EQ(registers(p, {0x0001}), unsigned_value(1));
    EXPECT_EQ(registers(p, {0x7FFF}), unsigned_value(32'767));
    EXPECT_EQ(registers(p, {0x8000}), unsigned_value(32'768));
    EXPECT_EQ(registers(p, {0xFFFF}), unsigned_value(65'535));
}

TEST(ModbusDecoder, DecodesInt16AsTwosComplement)
{
    const auto p = point(ModbusDataType::Int16);
    EXPECT_EQ(registers(p, {0x0000}), signed_value(0));
    EXPECT_EQ(registers(p, {0x0001}), signed_value(1));
    EXPECT_EQ(registers(p, {0xFFFF}), signed_value(-1));
    EXPECT_EQ(registers(p, {0xFF9C}), signed_value(-100));
    EXPECT_EQ(registers(p, {0x7FFF}), signed_value(std::numeric_limits<std::int16_t>::max()));
    EXPECT_EQ(registers(p, {0x8000}), signed_value(std::numeric_limits<std::int16_t>::min()));
}

TEST(ModbusDecoder, InputRegistersDecodeLikeHoldingRegisters)
{
    const auto p =
        point(ModbusDataType::Int16, std::nullopt, std::nullopt, 0, ModbusTable::InputRegisters);
    EXPECT_EQ(registers(p, {0xFFFE}, 0, ModbusTable::InputRegisters), signed_value(-2));
}

TEST(ModbusDecoder, RegisterOffsetIsRelativeToTheBlockStart)
{
    const auto p = point(ModbusDataType::UInt16, std::nullopt, std::nullopt, 103);
    EXPECT_EQ(registers(p, {10, 11, 12, 13, 14}, 100), unsigned_value(13));
}

TEST(ModbusDecoder, ReadsTheLastAddressOfTheTable)
{
    const auto p = point(ModbusDataType::UInt16, std::nullopt, std::nullopt, 65'535);
    EXPECT_EQ(registers(p, {0xBEEF}, 65'535), unsigned_value(0xBEEF));
}

// ---- 32-bit integers ---------------------------------------------------------------------------

TEST(ModbusDecoder, CombinesUInt32WordsInBothWordOrders)
{
    const std::vector<std::uint16_t> words{0x1234, 0x5678};
    EXPECT_EQ(registers(point(ModbusDataType::UInt32, kHigh), words), unsigned_value(0x12345678));
    EXPECT_EQ(registers(point(ModbusDataType::UInt32, kLow), words), unsigned_value(0x56781234));
}

TEST(ModbusDecoder, DecodesUInt32Extremes)
{
    for (const auto order : {kHigh, kLow}) {
        const auto p = point(ModbusDataType::UInt32, order);
        EXPECT_EQ(registers(p, {0x0000, 0x0000}), unsigned_value(0));
        EXPECT_EQ(registers(p, {0xFFFF, 0xFFFF}), unsigned_value(0xFFFFFFFFU));
    }
    // 65536 needs the high word: its position depends on the word order.
    EXPECT_EQ(registers(point(ModbusDataType::UInt32, kHigh), {0x0001, 0x0000}),
              unsigned_value(65'536));
    EXPECT_EQ(registers(point(ModbusDataType::UInt32, kLow), {0x0000, 0x0001}),
              unsigned_value(65'536));
}

TEST(ModbusDecoder, DecodesInt32AsTwosComplementInBothWordOrders)
{
    const auto high = point(ModbusDataType::Int32, kHigh);
    const auto low = point(ModbusDataType::Int32, kLow);
    EXPECT_EQ(registers(high, {0x0000, 0x0000}), signed_value(0));
    EXPECT_EQ(registers(high, {0xFFFF, 0xFFFF}), signed_value(-1));
    EXPECT_EQ(registers(high, {0xFFFF, 0xFFFE}), signed_value(-2));
    EXPECT_EQ(registers(low, {0xFFFE, 0xFFFF}), signed_value(-2));
    EXPECT_EQ(registers(high, {0x8000, 0x0000}),
              signed_value(std::numeric_limits<std::int32_t>::min()));
    EXPECT_EQ(registers(low, {0x0000, 0x8000}),
              signed_value(std::numeric_limits<std::int32_t>::min()));
    EXPECT_EQ(registers(high, {0x7FFF, 0xFFFF}),
              signed_value(std::numeric_limits<std::int32_t>::max()));
    EXPECT_EQ(registers(low, {0xFFFF, 0x7FFF}),
              signed_value(std::numeric_limits<std::int32_t>::max()));
}

TEST(ModbusDecoder, ReadsAThirtyTwoBitValueEndingAtTheLastAddress)
{
    const auto p = point(ModbusDataType::UInt32, kHigh, std::nullopt, 65'534);
    EXPECT_EQ(registers(p, {0x0001, 0x0002}, 65'534), unsigned_value(0x00010002));
}

TEST(ModbusDecoder, RejectsThirtyTwoBitValuesThatDoNotFitInTheBlock)
{
    const auto at_last = point(ModbusDataType::Int32, kHigh, std::nullopt, 2);
    EXPECT_EQ(registers(at_last, {1, 2, 3}), failed(DecodeError::InsufficientData));
    const auto at_end = point(ModbusDataType::Float32, kLow, std::nullopt, 65'535);
    EXPECT_EQ(registers(at_end, {0x3F80}, 65'535), failed(DecodeError::InsufficientData));
}

TEST(ModbusDecoder, RejectsRegistersOutsideTheBlock)
{
    const auto p = point(ModbusDataType::UInt16, std::nullopt, std::nullopt, 10);
    EXPECT_EQ(registers(p, {1, 2}, 11), failed(DecodeError::InsufficientData)); // before start
    EXPECT_EQ(registers(p, {1, 2}, 8), failed(DecodeError::InsufficientData));  // past the end
    EXPECT_EQ(registers(p, {}, 10), failed(DecodeError::InsufficientData));     // empty block
}

// ---- float32 -----------------------------------------------------------------------------------

TEST(ModbusDecoder, DecodesFloat32BitPatternsExactly)
{
    const auto p = point(ModbusDataType::Float32, kHigh);
    EXPECT_EQ(registers(p, words_of(0x3F800000U)), double_value(1.0));
    EXPECT_EQ(registers(p, words_of(0xC0200000U)), double_value(-2.5));
    EXPECT_EQ(registers(p, words_of(0x3E200000U)), double_value(0.15625));
    EXPECT_EQ(registers(p, words_of(0x40490FDBU)), double_value(kFloatPi));
}

TEST(ModbusDecoder, DecodesFloat32InBothWordOrders)
{
    // 0x40490FDB is float pi: "ABCD" for high_word_first, "CDAB" for low_word_first.
    const auto pi = double_value(kFloatPi);
    EXPECT_EQ(registers(point(ModbusDataType::Float32, kHigh), {0x4049, 0x0FDB}), pi);
    EXPECT_EQ(registers(point(ModbusDataType::Float32, kLow), {0x0FDB, 0x4049}), pi);
    // The wrong word order yields a different (tiny) number, never pi.
    EXPECT_NE(registers(point(ModbusDataType::Float32, kLow), {0x4049, 0x0FDB}), pi);
}

TEST(ModbusDecoder, DecodesFloat32ZerosAndExtremes)
{
    const auto p = point(ModbusDataType::Float32, kHigh);
    EXPECT_EQ(registers(p, words_of(0x00000000U)), double_value(0.0));
    const auto negative_zero = registers(p, words_of(0x80000000U));
    ASSERT_TRUE(negative_zero.ok());
    EXPECT_TRUE(std::signbit(std::get<double>(negative_zero.value())));
    EXPECT_EQ(registers(p, words_of(0x7F7FFFFFU)),
              double_value(static_cast<double>(std::numeric_limits<float>::max())));
    EXPECT_EQ(registers(p, words_of(0xFF7FFFFFU)),
              double_value(static_cast<double>(std::numeric_limits<float>::lowest())));
    EXPECT_EQ(registers(p, words_of(0x00800000U)),
              double_value(static_cast<double>(std::numeric_limits<float>::min())));
    EXPECT_EQ(registers(p, words_of(0x00000001U)),
              double_value(static_cast<double>(std::numeric_limits<float>::denorm_min())));
}

TEST(ModbusDecoder, RejectsNonFiniteFloat32)
{
    for (const auto pattern : {0x7F800000U, 0xFF800000U, 0x7FC00000U, 0xFFC00000U, 0x7F800001U}) {
        EXPECT_EQ(registers(point(ModbusDataType::Float32, kHigh), words_of(pattern)),
                  failed(DecodeError::NonFiniteValue))
            << std::hex << pattern;
    }
}

// ---- Scale -------------------------------------------------------------------------------------

TEST(ModbusDecoder, KeepsIntegersAsIntegersWithoutScale)
{
    EXPECT_TRUE(std::holds_alternative<std::uint64_t>(
        registers(point(ModbusDataType::UInt16), {7}).value()));
    EXPECT_TRUE(std::holds_alternative<std::int64_t>(
        registers(point(ModbusDataType::Int32, kHigh), {0, 7}).value()));
}

TEST(ModbusDecoder, ScaleProducesADoubleEvenForScaleOne)
{
    const auto result = registers(point(ModbusDataType::UInt16, std::nullopt, 1.0), {42});
    EXPECT_EQ(result, double_value(42.0));
}

TEST(ModbusDecoder, AppliesScaleAsRawTimesScale)
{
    EXPECT_DOUBLE_EQ(scaled(registers(point(ModbusDataType::UInt16, std::nullopt, 0.1), {75})),
                     7.5);
    EXPECT_DOUBLE_EQ(scaled(registers(point(ModbusDataType::Int16, std::nullopt, 0.5), {0xFF9C})),
                     -50.0);
    EXPECT_DOUBLE_EQ(scaled(registers(point(ModbusDataType::UInt16, std::nullopt, -2.0), {3})),
                     -6.0);
    EXPECT_DOUBLE_EQ(
        scaled(registers(point(ModbusDataType::UInt32, kLow, 0.001), {0x86A0, 0x0001})), 100.0);
    EXPECT_DOUBLE_EQ(scaled(registers(point(ModbusDataType::Int32, kHigh, 10.0), {0x8000, 0x0000})),
                     -21'474'836'480.0);
    EXPECT_DOUBLE_EQ(
        scaled(registers(point(ModbusDataType::Float32, kHigh, 4.0), words_of(0xC0200000U))),
        -10.0);
}

TEST(ModbusDecoder, ScalesZeroToZero)
{
    EXPECT_EQ(registers(point(ModbusDataType::Int16, std::nullopt, 0.1), {0}), double_value(0.0));
}

TEST(ModbusDecoder, RejectsScaledResultsThatOverflow)
{
    EXPECT_EQ(registers(point(ModbusDataType::Float32, kHigh, 1e300), words_of(0x7F7FFFFFU)),
              failed(DecodeError::NonFiniteValue));
    EXPECT_EQ(registers(point(ModbusDataType::UInt32, kHigh, 1e308), {0xFFFF, 0xFFFF}),
              failed(DecodeError::NonFiniteValue));
}

TEST(ModbusDecoder, RejectsInvalidScales)
{
    for (const double scale :
         {0.0, -0.0, std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_EQ(registers(point(ModbusDataType::UInt16, std::nullopt, scale), {1}),
                  failed(DecodeError::InvalidScale))
            << scale;
    }
}

// ---- Option and table checks -------------------------------------------------------------------

TEST(ModbusDecoder, RequiresWordOrderExactlyForThirtyTwoBitTypes)
{
    for (const auto type :
         {ModbusDataType::UInt32, ModbusDataType::Int32, ModbusDataType::Float32}) {
        EXPECT_EQ(registers(point(type), {0, 0}), failed(DecodeError::InvalidWordOrder));
    }
    for (const auto type : {ModbusDataType::UInt16, ModbusDataType::Int16}) {
        EXPECT_EQ(registers(point(type, kHigh), {0}), failed(DecodeError::InvalidWordOrder));
    }
}

TEST(ModbusDecoder, RejectsRegisterBlockFromAnotherTable)
{
    EXPECT_EQ(registers(point(ModbusDataType::UInt16), {1}, 0, ModbusTable::InputRegisters),
              failed(DecodeError::TableMismatch));
}

TEST(ModbusDecoder, RejectsBoolFromRegisters)
{
    const auto p = point(ModbusDataType::Bool);
    EXPECT_EQ(registers(p, {1}), failed(DecodeError::TypeTableMismatch));
}

TEST(ModbusDecoder, RejectsRegisterBlockForABitTable)
{
    const auto coil = bit_point(ModbusTable::Coils);
    EXPECT_EQ(registers(coil, {1}, 0, ModbusTable::Coils), failed(DecodeError::TypeTableMismatch));
}

// ---- Byte order and result API -----------------------------------------------------------------

TEST(ModbusDecoder, AssemblesRegistersFromBigEndianBytesIndependentOfTheCpu)
{
    static_assert(register_from_bytes(0x12, 0x34) == 0x1234);
    static_assert(register_from_bytes(0xFF, 0x00) == 0xFF00);
    static_assert(register_from_bytes(0x00, 0xFF) == 0x00FF);
    // PDU bytes 40 49 0F DB (float32 pi, high word first) on any host.
    const std::vector<std::uint16_t> words{register_from_bytes(0x40, 0x49),
                                           register_from_bytes(0x0F, 0xDB)};
    EXPECT_EQ(registers(point(ModbusDataType::Float32, kHigh), words), double_value(kFloatPi));
}

TEST(ModbusDecoder, ResultAccessorsEnforceTheirPreconditions)
{
    const auto success = DecodeResult::success(std::uint64_t{1});
    const auto failure = DecodeResult::failure(DecodeError::InvalidScale);
    EXPECT_TRUE(success.ok());
    EXPECT_FALSE(failure.ok());
    EXPECT_EQ(failure.error(), DecodeError::InvalidScale);
    EXPECT_THROW(static_cast<void>(failure.value()), std::logic_error);
    EXPECT_THROW(static_cast<void>(success.error()), std::logic_error);
}

TEST(ModbusDecoder, EveryErrorHasADistinctName)
{
    std::set<std::string_view> names;
    for (const auto error :
         {DecodeError::InsufficientData, DecodeError::TableMismatch, DecodeError::TypeTableMismatch,
          DecodeError::InvalidWordOrder, DecodeError::InvalidScale, DecodeError::NonFiniteValue}) {
        names.insert(to_string(error));
    }
    EXPECT_EQ(names.size(), 6U);
    EXPECT_EQ(names.count("unknown"), 0U);
}

} // namespace
} // namespace ieg::modbus
