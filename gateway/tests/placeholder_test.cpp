#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <string>

// Placeholder: verifies the test toolchain and third-party dependencies are wired up.
TEST(Placeholder, DependenciesAreUsable)
{
    const YAML::Node yaml = YAML::Load("mqtt:\n  qos: 1\n");
    EXPECT_EQ(yaml["mqtt"]["qos"].as<int>(), 1);

    const auto json = nlohmann::json::parse(R"({"quality": "GOOD"})");
    EXPECT_EQ(json.at("quality").get<std::string>(), "GOOD");
}
