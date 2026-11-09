#include "modbus/poll_plan.hpp"

#include <algorithm>
#include <numeric>
#include <tuple>

namespace ieg::modbus::detail {
namespace {

std::size_t first_address(const config::ModbusPointConfig& point) noexcept
{
    return point.offset;
}

// One past the last address the point covers.
std::size_t end_address(const config::ModbusPointConfig& point) noexcept
{
    return static_cast<std::size_t>(point.offset) + config::modbus_width(point.type);
}

std::size_t limit_for(config::ModbusTable table, const ReadLimits& limits) noexcept
{
    return config::is_bit_table(table) ? limits.max_bits : limits.max_registers;
}

ReadBlock block_for(std::size_t index, const config::ModbusPointConfig& point)
{
    return ReadBlock{.table = point.table,
                     .start = point.offset,
                     .count = end_address(point) - first_address(point),
                     .points = {index}};
}

} // namespace

std::vector<ReadBlock> plan_reads(const std::vector<PlannedPoint>& points, const ReadLimits& limits)
{
    std::vector<std::size_t> order(points.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, [&](std::size_t lhs, std::size_t rhs) {
        const auto& a = points[lhs].point;
        const auto& b = points[rhs].point;
        return std::tuple{a.table, a.offset} < std::tuple{b.table, b.offset};
    });

    std::vector<ReadBlock> blocks;
    for (const std::size_t index : order) {
        const auto& point = points[index].point;
        if (!blocks.empty()) {
            auto& block = blocks.back();
            const std::size_t block_end = block.start + block.count;
            const std::size_t new_end = std::max(block_end, end_address(point));
            // Contiguous or overlapping (no unconfigured gap) and within the request limit.
            if (block.table == point.table && first_address(point) <= block_end &&
                new_end - block.start <= limit_for(point.table, limits)) {
                block.count = new_end - block.start;
                block.points.push_back(index);
                continue;
            }
        }
        blocks.push_back(block_for(index, point));
    }
    return blocks;
}

std::vector<ReadBlock> split_block(const ReadBlock& block, const std::vector<PlannedPoint>& points)
{
    std::vector<ReadBlock> singles;
    singles.reserve(block.points.size());
    for (const std::size_t index : block.points) {
        singles.push_back(block_for(index, points[index].point));
    }
    return singles;
}

} // namespace ieg::modbus::detail
