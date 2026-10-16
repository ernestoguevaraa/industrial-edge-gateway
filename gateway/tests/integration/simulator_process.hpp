#pragma once

#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// POSIX process environment for posix_spawnp.
// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)
extern char** environ;

namespace ieg::testing {

// Runs `python -m plant_simulator` from the repository's simulator/ directory.
// The interpreter comes from IEG_SIMULATOR_PYTHON and must have the simulator's
// requirements (asyncua) installed.
class SimulatorProcess
{
public:
    static std::optional<std::string> python_from_environment()
    {
        const char* python = std::getenv("IEG_SIMULATOR_PYTHON"); // NOLINT(concurrency-mt-unsafe)
        if (python == nullptr || *python == '\0') {
            return std::nullopt;
        }
        return std::string{python};
    }

    static std::uint16_t free_port()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // NOLINTNEXTLINE(*-reinterpret-cast): BSD sockets API.
        auto* generic = reinterpret_cast<sockaddr*>(&address);
        socklen_t length = sizeof(address);
        if (fd < 0 || ::bind(fd, generic, length) != 0 ||
            ::getsockname(fd, generic, &length) != 0) {
            throw std::runtime_error("could not reserve a free port");
        }
        ::close(fd);
        return ntohs(address.sin_port);
    }

    SimulatorProcess(std::string python, std::uint16_t port)
        : python_(std::move(python)), port_(port)
    {}
    ~SimulatorProcess() { terminate(); }
    SimulatorProcess(const SimulatorProcess&) = delete;
    SimulatorProcess& operator=(const SimulatorProcess&) = delete;
    SimulatorProcess(SimulatorProcess&&) = delete;
    SimulatorProcess& operator=(SimulatorProcess&&) = delete;

    // Starts the simulator bound to 0.0.0.0 (so it advertises opc.tcp://0.0.0.0:<port>/) and
    // waits until the port accepts TCP connections.
    void launch()
    {
        const std::string port = std::to_string(port_);
        std::vector<std::string> args{
            python_,     "-m",  "plant_simulator", "--host", "0.0.0.0",     "--port", port,
            "--tick-ms", "100", "--log-format",    "text",   "--log-level", "WARNING"};
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        std::vector<std::string> env_storage{"PYTHONPATH=" IEG_SIMULATOR_DIR "/src",
                                             "PYTHONUNBUFFERED=1"};
        for (char** entry = environ; *entry != nullptr; ++entry) { // NOLINT(*-pointer-arithmetic)
            const std::string value{*entry};
            if (!value.starts_with("PYTHONPATH=")) {
                env_storage.push_back(value);
            }
        }
        std::vector<char*> envp;
        envp.reserve(env_storage.size() + 1);
        for (auto& entry : env_storage) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);

        if (::posix_spawnp(&pid_, python_.c_str(), nullptr, nullptr, argv.data(), envp.data()) !=
            0) {
            pid_ = -1;
            throw std::runtime_error("could not spawn simulator with " + python_);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (!accepts_connections()) {
            if (std::chrono::steady_clock::now() > deadline || !running()) {
                terminate();
                throw std::runtime_error("simulator did not open port " + port);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }

    // SIGTERM, escalating to SIGKILL after 5 s, then reap. asyncua 1.1.8 on Python 3.12 can
    // hang in Server.stop() if a client reconnects while it is shutting down (wait_closed()
    // waits for that new connection), so a clean exit is not guaranteed.
    void terminate()
    {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        int status = 0;
        while (::waitpid(pid_, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::cerr << "[simulator] SIGTERM did not stop pid " << pid_
                          << " within 5 s; sending SIGKILL\n";
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        pid_ = -1;
    }

    [[nodiscard]] std::string endpoint() const
    {
        return "opc.tcp://127.0.0.1:" + std::to_string(port_);
    }

private:
    [[nodiscard]] bool running() const
    {
        int status = 0;
        return pid_ > 0 && ::waitpid(pid_, &status, WNOHANG) == 0;
    }

    [[nodiscard]] bool accepts_connections() const
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        // NOLINTNEXTLINE(*-reinterpret-cast): BSD sockets API.
        const auto* generic = reinterpret_cast<const sockaddr*>(&address);
        const bool ok = fd >= 0 && ::connect(fd, generic, sizeof(address)) == 0;
        if (fd >= 0) {
            ::close(fd);
        }
        return ok;
    }

    std::string python_;
    std::uint16_t port_;
    pid_t pid_{-1};
};

} // namespace ieg::testing
