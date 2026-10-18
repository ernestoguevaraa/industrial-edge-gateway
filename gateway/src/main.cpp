#include "ieg/config/config_loader.hpp"
#include "ieg/runtime/gateway_runtime.hpp"

#include <pthread.h>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string_view>

namespace {

// Exit codes: 0 after a requested shutdown, 1 runtime start failure, 2 configuration/usage error.
constexpr int kExitOk = 0;
constexpr int kExitStartFailure = 1;
constexpr int kExitConfigError = 2;

void configure_logging()
{
    spdlog::set_pattern("%Y-%m-%dT%H:%M:%S.%eZ %l [%n] %v", spdlog::pattern_time_type::utc);
    const char* level =
        std::getenv("IEG_LOG_LEVEL"); // NOLINT(concurrency-mt-unsafe): no threads yet
    spdlog::set_level(level != nullptr ? spdlog::level::from_str(level) : spdlog::level::info);
}

std::string_view signal_name(int signal_number)
{
    switch (signal_number) {
    case SIGINT:
        return "SIGINT";
    case SIGTERM:
        return "SIGTERM";
    default:
        return "unknown";
    }
}

} // namespace

int main(int argc, char** argv)
{
    // Block SIGINT/SIGTERM before any component can create a thread, so every thread inherits
    // the mask and the signals are only received by sigwait() below.
    sigset_t shutdown_signals{};
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr) != 0) {
        return kExitStartFailure;
    }

    configure_logging();
    if (argc != 2) {
        spdlog::error("event=usage message=\"usage: gateway <config.yaml>\"");
        return kExitConfigError;
    }
    const std::string_view config_path{argv[1]}; // NOLINT(*-pointer-arithmetic): argv access

    std::unique_ptr<ieg::runtime::GatewayRuntime> runtime;
    try {
        const auto config = ieg::config::load_config_file(config_path);
        runtime = ieg::runtime::make_gateway_runtime(config);
    } catch (const ieg::config::ConfigError& error) {
        for (const auto& problem : error.errors()) {
            spdlog::error("event=config_error config={} problem=\"{}\"", config_path, problem);
        }
        return kExitConfigError;
    } catch (const ieg::runtime::StartupError& error) {
        spdlog::error("event=startup_validation_failed config={} problem=\"{}\"", config_path,
                      error.what());
        return kExitConfigError;
    }

    try {
        runtime->start();
    } catch (const std::exception& error) {
        spdlog::critical("event=start_failed error=\"{}\"", error.what());
        return kExitStartFailure;
    }

    int received = 0;
    sigwait(&shutdown_signals, &received);
    spdlog::info("event=shutdown_requested signal={}", signal_name(received));
    runtime->stop();
    return kExitOk;
}
