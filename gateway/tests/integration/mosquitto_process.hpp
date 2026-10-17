#pragma once

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// POSIX process environment for posix_spawn.
// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)
extern char** environ;

namespace ieg::testing {

// Runs a real Mosquitto broker on a loopback port. The binary comes from IEG_MOSQUITTO.
class MosquittoProcess
{
public:
    static std::optional<std::string> binary_from_environment()
    {
        const char* binary = std::getenv("IEG_MOSQUITTO"); // NOLINT(concurrency-mt-unsafe)
        if (binary == nullptr || *binary == '\0') {
            return std::nullopt;
        }
        return std::string{binary};
    }

    // `acl` (optional) is written to an ACL file referenced by the broker configuration.
    MosquittoProcess(std::string binary, std::uint16_t port, const std::string& acl = {})
        : binary_(std::move(binary)), port_(port)
    {
        std::array<char, 32> pattern{"/tmp/ieg-mosquitto-XXXXXX"};
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("could not create a temporary directory for mosquitto");
        }
        directory_ = pattern.data();
        std::ofstream config{directory_ / "mosquitto.conf"};
        config << "listener " << port_ << " 127.0.0.1\n"
               << "allow_anonymous true\n"
               << "persistence false\n"
               << "log_dest none\n";
        if (!acl.empty()) {
            std::ofstream{directory_ / "acl"} << acl;
            config << "acl_file " << (directory_ / "acl").string() << "\n";
        }
    }

    ~MosquittoProcess()
    {
        terminate();
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    MosquittoProcess(const MosquittoProcess&) = delete;
    MosquittoProcess& operator=(const MosquittoProcess&) = delete;
    MosquittoProcess(MosquittoProcess&&) = delete;
    MosquittoProcess& operator=(MosquittoProcess&&) = delete;

    void launch()
    {
        std::string config = (directory_ / "mosquitto.conf").string();
        std::string flag = "-c";
        std::vector<char*> argv{binary_.data(), flag.data(), config.data(), nullptr};
        posix_spawn_file_actions_t actions{};
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        const int rc =
            ::posix_spawn(&pid_, binary_.c_str(), &actions, nullptr, argv.data(), environ);
        ::posix_spawn_file_actions_destroy(&actions);
        if (rc != 0) {
            pid_ = -1;
            throw std::runtime_error("could not spawn " + binary_);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!accepts_connections()) {
            if (std::chrono::steady_clock::now() > deadline) {
                terminate();
                throw std::runtime_error("mosquitto did not open port " + std::to_string(port_));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }

    // SIGTERM (escalating to SIGKILL after 5 s), then reap.
    void terminate() { stop_with(SIGTERM); }
    // Abrupt death: the kernel closes the broker's sockets.
    void kill() { stop_with(SIGKILL); }
    // Freezes the broker: TCP connections stay open but nothing is answered.
    void pause() const { signal(SIGSTOP); }
    void resume() const { signal(SIGCONT); }

    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void signal(int signum) const
    {
        if (pid_ > 0) {
            ::kill(pid_, signum);
        }
    }

    void stop_with(int signum)
    {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, signum);
        ::kill(pid_, SIGCONT); // a paused broker must run to handle SIGTERM
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        int status = 0;
        while (::waitpid(pid_, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::cerr << "[mosquitto] pid " << pid_ << " ignored signal; sending SIGKILL\n";
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        pid_ = -1;
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

    std::string binary_;
    std::uint16_t port_;
    std::filesystem::path directory_;
    pid_t pid_{-1};
};

} // namespace ieg::testing
