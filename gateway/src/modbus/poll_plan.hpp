#pragma once

#include "ieg/acquisition/acquisition_source.hpp"
#include "ieg/config/gateway_config.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

// Read planning for the Modbus poller: which requests one polling cycle sends. Private to the
// Modbus source and its unit tests.
namespace ieg::modbus::detail {

struct PlannedPoint
{
    acquisition::ItemId item{};
    config::ModbusPointConfig point;
};

// One request: `count` consecutive addresses of `table` starting at `start`, covering the points
// with the given indices (into the PlannedPoint list), in address order.
struct ReadBlock
{
    config::ModbusTable table{config::ModbusTable::HoldingRegisters};
    std::uint16_t start{};
    std::size_t count{};
    std::vector<std::size_t> points;

    friend bool operator==(const ReadBlock&, const ReadBlock&) = default;
};

struct ReadLimits
{
    std::size_t max_registers{125};
    std::size_t max_bits{2000};
};

// Coalesces points into as few requests as possible without ever reading an address that no
// point covers: points of the same table are merged while they are contiguous or overlapping
// and the request stays within the limit for its table. Blocks are ordered by table, then
// address. Each point always fits (its width is at most 2).
[[nodiscard]] std::vector<ReadBlock> plan_reads(const std::vector<PlannedPoint>& points,
                                                const ReadLimits& limits);

// One block per point of `block` (used after a block answered with a Modbus exception).
[[nodiscard]] std::vector<ReadBlock> split_block(const ReadBlock& block,
                                                 const std::vector<PlannedPoint>& points);

// Deadline of the next polling cycle on the fixed grid previous + k * interval (k >= 1): the
// first slot at or after `now`. A cycle that overran one or more slots skips them instead of
// running them back to back (no catch-up burst), and the grid keeps its phase.
template <typename TimePoint, typename Duration>
[[nodiscard]] TimePoint next_poll_deadline(TimePoint previous, TimePoint now, Duration interval)
{
    auto next = previous + interval;
    if (next < now) {
        auto slots = (now - previous) / interval; // whole slots that already passed
        if (previous + (slots * interval) < now) {
            ++slots;
        }
        next = previous + (slots * interval);
    }
    return next;
}

} // namespace ieg::modbus::detail
