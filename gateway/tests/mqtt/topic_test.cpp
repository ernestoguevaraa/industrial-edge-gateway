#include "mqtt/topic.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace ieg::mqtt::detail {
namespace {

using namespace std::string_literals;

TEST(MqttTopic, BuildsTopicFromPrefixAssetLevelsAndTag)
{
    const auto topic = make_topic("industrial", "site01.line01.press01", "pressure");
    ASSERT_TRUE(topic.ok()) << topic.error;
    EXPECT_EQ(topic.name, "industrial/site01/line01/press01/pressure");
}

TEST(MqttTopic, SupportsMultiLevelPrefixAndLongerAssetPaths)
{
    const auto topic = make_topic("plant/industrial", "site01.area2.line01.cell3.oven01", "temp");
    ASSERT_TRUE(topic.ok()) << topic.error;
    EXPECT_EQ(topic.name, "plant/industrial/site01/area2/line01/cell3/oven01/temp");
}

TEST(MqttTopic, KeepsNamesExactlyAsConfigured)
{
    const auto topic = make_topic("industrial", "Site01.Line01.Oven01", "Temperature");
    ASSERT_TRUE(topic.ok()) << topic.error;
    EXPECT_EQ(topic.name, "industrial/Site01/Line01/Oven01/Temperature");
}

TEST(MqttTopic, RejectsEmptyAssetLevelsAndTag)
{
    EXPECT_FALSE(make_topic("industrial", "", "pressure").ok());
    EXPECT_FALSE(make_topic("industrial", "site01..press01", "pressure").ok());
    EXPECT_FALSE(make_topic("industrial", ".site01", "pressure").ok());
    EXPECT_FALSE(make_topic("industrial", "site01.", "pressure").ok());
    EXPECT_FALSE(make_topic("industrial", "site01.line01.press01", "").ok());
}

TEST(MqttTopic, RejectsSeparatorsWildcardsAndNulInSegments)
{
    for (const std::string& bad : {"a/b"s, "a+b"s, "a#b"s, "a\0b"s}) {
        SCOPED_TRACE(bad);
        EXPECT_FALSE(make_topic("industrial", "site01." + bad + ".press01", "pressure").ok());
        EXPECT_FALSE(make_topic("industrial", "site01.line01.press01", bad).ok());
    }
}

TEST(MqttTopic, RejectsInvalidUtf8)
{
    EXPECT_FALSE(make_topic("industrial", "site01.line\xC3.press01", "pressure").ok());
    EXPECT_FALSE(make_topic("industrial", "site01.line01.press01", "\xFF").ok());
    const auto valid = make_topic("industrial", "site01.línea01.presse01", "druck°");
    EXPECT_TRUE(valid.ok()) << valid.error;
}

TEST(MqttTopic, RejectsTopicsLongerThan65535Bytes)
{
    const std::string tag(kMaxTopicBytes, 't');
    EXPECT_FALSE(make_topic("industrial", "site01.line01.press01", tag).ok());
    const std::string prefix = "p";
    const std::string fitting_tag(kMaxTopicBytes - prefix.size() - std::string{"/a/"}.size(), 't');
    const auto fits = make_topic(prefix, "a", fitting_tag);
    ASSERT_TRUE(fits.ok()) << fits.error;
    EXPECT_EQ(fits.name.size(), kMaxTopicBytes);
}

TEST(MqttTopicPrefix, AcceptsOneOrMoreValidLevels)
{
    EXPECT_EQ(topic_prefix_error("industrial"), "");
    EXPECT_EQ(topic_prefix_error("plant/industrial/v1"), "");
}

TEST(MqttTopicPrefix, RejectsEmptyLevelsWildcardsNulAndInvalidUtf8)
{
    for (const std::string& bad : {""s, "/industrial"s, "industrial/"s, "a//b"s, "/"s, "a/+/b"s,
                                   "a/#"s, "in+dustrial"s, "a\0b"s, "\xC3\x28"s}) {
        SCOPED_TRACE(bad);
        EXPECT_NE(topic_prefix_error(bad), "");
    }
}

TEST(MqttUtf8, ValidatesWellFormedUtf8WithoutNul)
{
    EXPECT_TRUE(is_valid_mqtt_utf8(""));
    EXPECT_TRUE(is_valid_mqtt_utf8("ascii"));
    EXPECT_TRUE(is_valid_mqtt_utf8("\xC3\xA9"));         // é
    EXPECT_TRUE(is_valid_mqtt_utf8("\xE2\x82\xAC"));     // €
    EXPECT_TRUE(is_valid_mqtt_utf8("\xF0\x9F\x8F\xAD")); // U+1F3ED
    EXPECT_FALSE(is_valid_mqtt_utf8("a\0b"s));
    EXPECT_FALSE(is_valid_mqtt_utf8("\xC0\xAF"));         // overlong '/'
    EXPECT_FALSE(is_valid_mqtt_utf8("\xED\xA0\x80"));     // surrogate U+D800
    EXPECT_FALSE(is_valid_mqtt_utf8("\xF4\x90\x80\x80")); // above U+10FFFF
    EXPECT_FALSE(is_valid_mqtt_utf8("\xE2\x82"));         // truncated
    EXPECT_FALSE(is_valid_mqtt_utf8("\x80"));             // stray continuation byte
}

} // namespace
} // namespace ieg::mqtt::detail
