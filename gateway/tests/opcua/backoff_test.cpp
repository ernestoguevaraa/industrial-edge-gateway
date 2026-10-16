#include "opcua/backoff.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>

namespace ieg::opcua::detail {
namespace {

using namespace std::chrono_literals;

TEST(Backoff, DoublesFromInitialDelay)
{
    EXPECT_EQ(backoff_delay(0, 500ms, 30s), 500ms);
    EXPECT_EQ(backoff_delay(1, 500ms, 30s), 1000ms);
    EXPECT_EQ(backoff_delay(2, 500ms, 30s), 2000ms);
    EXPECT_EQ(backoff_delay(5, 500ms, 30s), 16000ms);
}

TEST(Backoff, IsCappedAtMaximum)
{
    EXPECT_EQ(backoff_delay(6, 500ms, 30s), 30s);
    EXPECT_EQ(backoff_delay(7, 500ms, 30s), 30s);
    EXPECT_EQ(backoff_delay(std::numeric_limits<unsigned>::max(), 500ms, 30s), 30s);
}

TEST(Backoff, InitialNotBelowMaximumReturnsMaximum)
{
    EXPECT_EQ(backoff_delay(0, 30s, 30s), 30s);
    EXPECT_EQ(backoff_delay(3, 60s, 30s), 30s);
}

static_assert(backoff_delay(3, 1ms, 1000ms) == 8ms);

} // namespace
} // namespace ieg::opcua::detail
