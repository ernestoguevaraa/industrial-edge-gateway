#include "modbus/poll_plan.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ieg::modbus::detail {
namespace {

using namespace std::chrono_literals;
using config::ModbusDataType;
using config::ModbusTable;
using config::ModbusWordOrder;

PlannedPoint planned(acquisition::ItemId item, ModbusTable table, std::uint16_t offset,
                     ModbusDataType type = ModbusDataType::UInt16)
{
    config::ModbusPointConfig point;
    point.asset = "site01.line01.comp01";
    point.tag = "t" + std::to_string(item);
    point.table = table;
    point.offset = offset;
    point.type = type;
    if (config::modbus_width(type) == 2) {
        point.word_order = ModbusWordOrder::HighWordFirst;
    }
    return PlannedPoint{.item = item, .point = point};
}

ReadBlock block(ModbusTable table, std::uint16_t start, std::size_t count,
                std::vector<std::size_t> points)
{
    return ReadBlock{.table = table, .start = start, .count = count, .points = std::move(points)};
}

constexpr ReadLimits kProtocolLimits{.max_registers = 125, .max_bits = 2000};

TEST(PollPlan, MergesContiguousPointsOfOneTable)
{
    const std::vector<PlannedPoint> points{
        planned(1, ModbusTable::HoldingRegisters, 10),
        planned(2, ModbusTable::HoldingRegisters, 11),
        planned(3, ModbusTable::HoldingRegisters, 12, ModbusDataType::Float32), // 12..13
        planned(4, ModbusTable::HoldingRegisters, 14),
    };
    EXPECT_EQ(plan_reads(points, kProtocolLimits),
              (std::vector<ReadBlock>{block(ModbusTable::HoldingRegisters, 10, 5, {0, 1, 2, 3})}));
}

TEST(PollPlan, NeverReadsAcrossAnUnconfiguredGap)
{
    const std::vector<PlannedPoint> points{
        planned(1, ModbusTable::HoldingRegisters, 10),
        planned(2, ModbusTable::HoldingRegisters, 12), // 11 is not configured
    };
    EXPECT_EQ(plan_reads(points, kProtocolLimits),
              (std::vector<ReadBlock>{block(ModbusTable::HoldingRegisters, 10, 1, {0}),
                                      block(ModbusTable::HoldingRegisters, 12, 1, {1})}));
}

TEST(PollPlan, NeverMergesDifferentTables)
{
    const std::vector<PlannedPoint> points{
        planned(1, ModbusTable::HoldingRegisters, 0),
        planned(2, ModbusTable::InputRegisters, 1),
        planned(3, ModbusTable::Coils, 2, ModbusDataType::Bool),
    };
    const auto blocks = plan_reads(points, kProtocolLimits);
    ASSERT_EQ(blocks.size(), 3U);
    EXPECT_EQ(blocks[0].table, ModbusTable::Coils); // ordered by table, then address
    EXPECT_EQ(blocks[1].table, ModbusTable::HoldingRegisters);
    EXPECT_EQ(blocks[2].table, ModbusTable::InputRegisters);
}

TEST(PollPlan, MergesOverlappingPointsWithoutDuplicatingAddresses)
{
    const std::vector<PlannedPoint> points{
        planned(1, ModbusTable::InputRegisters, 5, ModbusDataType::UInt32), // 5..6
        planned(2, ModbusTable::InputRegisters, 5),                         // 5 (same register)
        planned(3, ModbusTable::InputRegisters, 6),                         // 6
    };
    EXPECT_EQ(plan_reads(points, kProtocolLimits),
              (std::vector<ReadBlock>{block(ModbusTable::InputRegisters, 5, 2, {0, 1, 2})}));
}

TEST(PollPlan, SortsByAddressRegardlessOfConfigurationOrder)
{
    const std::vector<PlannedPoint> points{
        planned(1, ModbusTable::HoldingRegisters, 21),
        planned(2, ModbusTable::HoldingRegisters, 20),
    };
    EXPECT_EQ(plan_reads(points, kProtocolLimits),
              (std::vector<ReadBlock>{block(ModbusTable::HoldingRegisters, 20, 2, {1, 0})}));
}

TEST(PollPlan, SplitsAtTheRegisterLimitWithoutSplittingAPoint)
{
    std::vector<PlannedPoint> points;
    for (std::uint16_t offset = 0; offset < 5; ++offset) {
        points.push_back(planned(offset, ModbusTable::HoldingRegisters, offset));
    }
    points.push_back(planned(9, ModbusTable::HoldingRegisters, 5, ModbusDataType::Int32)); // 5..6
    const auto blocks = plan_reads(points, {.max_registers = 6, .max_bits = 2000});
    EXPECT_EQ(blocks,
              (std::vector<ReadBlock>{block(ModbusTable::HoldingRegisters, 0, 5, {0, 1, 2, 3, 4}),
                                      block(ModbusTable::HoldingRegisters, 5, 2, {5})}));
}

// `count` consecutive single-address points of `table` starting at address 0.
std::vector<PlannedPoint> consecutive(ModbusTable table, std::uint32_t count, ModbusDataType type)
{
    std::vector<PlannedPoint> points;
    for (std::uint32_t offset = 0; offset < count; ++offset) {
        points.push_back(planned(offset, table, static_cast<std::uint16_t>(offset), type));
    }
    return points;
}

std::vector<std::size_t> block_sizes(const std::vector<ReadBlock>& blocks)
{
    std::vector<std::size_t> sizes;
    sizes.reserve(blocks.size());
    for (const auto& block : blocks) {
        sizes.push_back(block.count);
    }
    return sizes;
}

TEST(PollPlan, RespectsTheProtocolMaximumForRegisters)
{
    const auto points = consecutive(ModbusTable::InputRegisters, 300, ModbusDataType::UInt16);
    EXPECT_EQ(block_sizes(plan_reads(points, kProtocolLimits)),
              (std::vector<std::size_t>{125, 125, 50}));
}

TEST(PollPlan, RespectsTheProtocolMaximumForBits)
{
    const auto points = consecutive(ModbusTable::DiscreteInputs, 4500, ModbusDataType::Bool);
    EXPECT_EQ(block_sizes(plan_reads(points, kProtocolLimits)),
              (std::vector<std::size_t>{2000, 2000, 500}));
}

TEST(PollPlan, SplitsABlockIntoOneReadPerPoint)
{
    const std::vector<PlannedPoint> points{
        planned(1, ModbusTable::HoldingRegisters, 10),
        planned(2, ModbusTable::HoldingRegisters, 11, ModbusDataType::Float32),
    };
    const auto merged = plan_reads(points, kProtocolLimits);
    ASSERT_EQ(merged.size(), 1U);
    EXPECT_EQ(split_block(merged[0], points),
              (std::vector<ReadBlock>{block(ModbusTable::HoldingRegisters, 10, 1, {0}),
                                      block(ModbusTable::HoldingRegisters, 11, 2, {1})}));
}

TEST(PollPlan, HandlesTheEndOfTheAddressSpace)
{
    const std::vector<PlannedPoint> points{
        planned(1, ModbusTable::HoldingRegisters, 65'534, ModbusDataType::UInt32),
    };
    EXPECT_EQ(plan_reads(points, kProtocolLimits),
              (std::vector<ReadBlock>{block(ModbusTable::HoldingRegisters, 65'534, 2, {0})}));
}

// ---- Scheduling ------------------------------------------------------------------------------

using TimePoint = std::chrono::steady_clock::time_point;
constexpr TimePoint kT0{};

TEST(PollSchedule, NextDeadlineIsOneIntervalLaterWhenTheCycleWasShort)
{
    EXPECT_EQ(next_poll_deadline(kT0, kT0 + 30ms, 100ms), kT0 + 100ms);
    EXPECT_EQ(next_poll_deadline(kT0, kT0 + 100ms, 100ms), kT0 + 100ms); // exactly on time
}

TEST(PollSchedule, SkipsMissedSlotsAfterAnOverrunAndKeepsThePhase)
{
    EXPECT_EQ(next_poll_deadline(kT0, kT0 + 101ms, 100ms), kT0 + 200ms);
    EXPECT_EQ(next_poll_deadline(kT0, kT0 + 250ms, 100ms), kT0 + 300ms);
    EXPECT_EQ(next_poll_deadline(kT0, kT0 + 300ms, 100ms), kT0 + 300ms);
}

} // namespace
} // namespace ieg::modbus::detail
