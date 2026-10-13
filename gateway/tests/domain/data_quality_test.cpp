#include "ieg/domain/data_quality.hpp"

#include <gtest/gtest.h>

namespace ieg::domain {
namespace {

TEST(DataQuality, ToStringUsesUppercaseIdentifiers)
{
    EXPECT_EQ(to_string(DataQuality::Good), "GOOD");
    EXPECT_EQ(to_string(DataQuality::Uncertain), "UNCERTAIN");
    EXPECT_EQ(to_string(DataQuality::Bad), "BAD");
}

static_assert(to_string(DataQuality::Good) == "GOOD");

} // namespace
} // namespace ieg::domain
