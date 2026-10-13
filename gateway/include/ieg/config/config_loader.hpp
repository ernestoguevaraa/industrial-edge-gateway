#pragma once

#include "ieg/config/gateway_config.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ieg::config {

// Carries every configuration problem found; what() lists them one per line.
class ConfigError : public std::runtime_error
{
public:
    explicit ConfigError(std::vector<std::string> errors);

    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return *errors_; }

private:
    // Shared so that copying the exception cannot throw.
    std::shared_ptr<const std::vector<std::string>> errors_;
};

// Both functions return a fully validated configuration or throw ConfigError.
[[nodiscard]] GatewayConfig load_config_file(const std::filesystem::path& path);
[[nodiscard]] GatewayConfig parse_config(std::string_view yaml,
                                         std::string_view source_name = "<memory>");

} // namespace ieg::config
