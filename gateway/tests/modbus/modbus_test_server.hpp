#pragma once

// Deterministic in-process Modbus TCP server for client tests.
//
// It speaks the Modbus TCP framing (MBAP header + PDU) for function codes 01-04 with plain POSIX
// sockets on 127.0.0.1 and an ephemeral port, so the tests need no external process and do not
// exercise libmodbus' own server code. The behaviour of the next requests can be scripted
// (normal reply, exception, silence, connection close, malformed reply).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ieg::modbus::testing {

class ModbusTestServer
{
public:
    enum class Mode : std::uint8_t
    {
        Normal,             // correct response from the in-memory tables
        Exception,          // exception response with exception_code()
        Silent,             // read the request, never answer (client timeout)
        CloseOnRequest,     // close the connection instead of answering
        WrongTransactionId, // correct data but a different transaction ID
        TruncatedResponse,  // one value fewer than requested, consistently framed
        Trickle,            // correct response, sent one byte every kTrickleDelay
    };

    ModbusTestServer() : listener_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        if (listener_ < 0) {
            throw std::runtime_error("socket() failed");
        }
        const int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX sockaddr API.
        auto* generic = reinterpret_cast<sockaddr*>(&address);
        socklen_t length = sizeof(address);
        if (::bind(listener_, generic, sizeof(address)) != 0 || ::listen(listener_, 4) != 0 ||
            ::getsockname(listener_, generic, &length) != 0) {
            ::close(listener_);
            throw std::runtime_error("cannot listen on 127.0.0.1");
        }
        port_ = ntohs(address.sin_port);
        holding_.resize(kTableSize);
        input_.resize(kTableSize);
        coils_.resize(kTableSize);
        discrete_.resize(kTableSize);
        thread_ = std::thread([this] { run(); });
    }

    ~ModbusTestServer()
    {
        stop_ = true;
        thread_.join();
        close_client();
        ::close(listener_);
    }

    ModbusTestServer(const ModbusTestServer&) = delete;
    ModbusTestServer& operator=(const ModbusTestServer&) = delete;
    ModbusTestServer(ModbusTestServer&&) = delete;
    ModbusTestServer& operator=(ModbusTestServer&&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    void set_mode(Mode mode, std::uint8_t exception_code = 0x02)
    {
        const std::lock_guard lock(mutex_);
        mode_ = mode;
        exception_code_ = exception_code;
    }

    void set_holding(std::uint16_t address, std::uint16_t value) { set(holding_, address, value); }
    void set_input(std::uint16_t address, std::uint16_t value) { set(input_, address, value); }
    void set_coil(std::uint16_t address, bool value) { set(coils_, address, value ? 1U : 0U); }
    void set_discrete(std::uint16_t address, bool value)
    {
        set(discrete_, address, value ? 1U : 0U);
    }

    // Closes the active client connection from the server side (asynchronously). Wait for
    // accepted() first: connect() can return before the server has accepted the connection.
    void drop_connection() { drop_requested_ = true; }

    [[nodiscard]] int accepted() const noexcept { return accepted_.load(); }
    [[nodiscard]] int closed_by_peer() const noexcept { return closed_by_peer_.load(); }
    [[nodiscard]] int dropped() const noexcept { return dropped_.load(); }
    [[nodiscard]] int requests() const noexcept { return requests_.load(); }
    [[nodiscard]] int last_unit_id() const noexcept { return last_unit_id_.load(); }
    [[nodiscard]] int last_function() const noexcept { return last_function_.load(); }

    // Polls `predicate` until it holds or `timeout` passes.
    template <typename Predicate>
    static bool wait_until(Predicate predicate,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{2000})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return true;
    }

private:
    static constexpr std::size_t kTableSize = 65'536;
    static constexpr std::size_t kHeaderSize = 7; // MBAP: transaction, protocol, length, unit
    static constexpr auto kTrickleDelay = std::chrono::milliseconds{50};

    template <typename T>
    void set(std::vector<T>& table, std::uint16_t address, unsigned value)
    {
        const std::lock_guard lock(mutex_);
        table[address] = static_cast<T>(value);
    }

    void run()
    {
        while (!stop_) {
            if (drop_requested_.exchange(false) && client_ >= 0) {
                close_client();
                ++dropped_;
            }
            std::array<pollfd, 2> fds{pollfd{.fd = listener_, .events = POLLIN, .revents = 0},
                                      pollfd{.fd = client_, .events = POLLIN, .revents = 0}};
            if (::poll(fds.data(), client_ >= 0 ? 2 : 1, 10) <= 0) {
                continue;
            }
            if ((fds[0].revents & POLLIN) != 0) {
                accept_client();
            } else if (client_ >= 0 && fds[1].revents != 0) {
                serve_one();
            }
        }
    }

    void accept_client()
    {
        const int fd = ::accept(listener_, nullptr, nullptr);
        if (fd >= 0) {
            // One connection at a time. Count the previous one as closed by the peer if the
            // client already closed it (a reconnect closes the old socket before connecting).
            std::uint8_t byte{};
            if (client_ >= 0 && ::recv(client_, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
                ++closed_by_peer_;
            }
            close_client();
            client_ = fd;
            ++accepted_;
        }
    }

    void close_client()
    {
        if (client_ >= 0) {
            ::close(client_);
            client_ = -1;
        }
    }

    bool receive_exact(std::uint8_t* data, std::size_t size) const
    {
        const auto received = ::recv(client_, data, size, MSG_WAITALL);
        return received == static_cast<ssize_t>(size);
    }

    void serve_one()
    {
        std::array<std::uint8_t, kHeaderSize> header{};
        if (!receive_exact(header.data(), header.size())) {
            ++closed_by_peer_;
            close_client();
            return;
        }
        const std::size_t length = (static_cast<std::size_t>(header[4]) << 8U) | header[5];
        if (length < 2 || length > 254) {
            close_client();
            return;
        }
        std::vector<std::uint8_t> pdu(length - 1);
        if (!receive_exact(pdu.data(), pdu.size())) {
            ++closed_by_peer_;
            close_client();
            return;
        }
        ++requests_;
        last_unit_id_ = header[6];
        last_function_ = pdu[0];
        respond(header, pdu);
    }

    void respond(const std::array<std::uint8_t, kHeaderSize>& header,
                 const std::vector<std::uint8_t>& request)
    {
        Mode mode{};
        std::uint8_t exception_code{};
        {
            const std::lock_guard lock(mutex_);
            mode = mode_;
            exception_code = exception_code_;
        }
        switch (mode) {
        case Mode::Silent:
            return;
        case Mode::CloseOnRequest:
            close_client();
            return;
        case Mode::Exception:
            send_pdu(header, {static_cast<std::uint8_t>(request[0] | 0x80U), exception_code},
                     false);
            return;
        case Mode::Normal:
        case Mode::WrongTransactionId:
        case Mode::TruncatedResponse:
        case Mode::Trickle:
            break;
        }
        auto pdu = read_response(request);
        if (mode == Mode::TruncatedResponse && pdu.size() > 3) {
            // One value fewer than requested, with a byte count (and MBAP length) that matches
            // the shortened data, so the frame is complete but does not answer the request.
            pdu.resize(pdu.size() - (request[0] <= 2 ? 1U : 2U));
            pdu[1] = static_cast<std::uint8_t>(pdu.size() - 2);
        }
        send_pdu(header, pdu, mode == Mode::WrongTransactionId, mode == Mode::Trickle);
    }

    std::vector<std::uint8_t> read_response(const std::vector<std::uint8_t>& request)
    {
        const std::uint8_t function = request[0];
        const std::size_t start = (static_cast<std::size_t>(request[1]) << 8U) | request[2];
        const std::size_t quantity = (static_cast<std::size_t>(request[3]) << 8U) | request[4];
        if (request.size() != 5 || function < 1 || function > 4 || start + quantity > kTableSize) {
            return {static_cast<std::uint8_t>(function | 0x80U), 0x01};
        }
        const std::lock_guard lock(mutex_);
        std::vector<std::uint8_t> pdu{function, 0};
        if (function <= 2) {
            const auto& table = function == 1 ? coils_ : discrete_;
            pdu.resize(2 + ((quantity + 7) / 8), 0);
            for (std::size_t i = 0; i < quantity; ++i) {
                if (table[start + i] != 0U) {
                    pdu[2 + (i / 8)] |= static_cast<std::uint8_t>(1U << (i % 8));
                }
            }
        } else {
            const auto& table = function == 3 ? holding_ : input_;
            for (std::size_t i = 0; i < quantity; ++i) {
                pdu.push_back(static_cast<std::uint8_t>(table[start + i] >> 8U));
                pdu.push_back(static_cast<std::uint8_t>(table[start + i] & 0xFFU));
            }
        }
        pdu[1] = static_cast<std::uint8_t>(pdu.size() - 2);
        return pdu;
    }

    void send_pdu(const std::array<std::uint8_t, kHeaderSize>& request_header,
                  const std::vector<std::uint8_t>& pdu, bool wrong_transaction,
                  bool trickle = false)
    {
        const std::size_t length = pdu.size() + 1;
        std::vector<std::uint8_t> frame{
            request_header[0],
            static_cast<std::uint8_t>(request_header[1] + (wrong_transaction ? 1U : 0U)),
            0,
            0,
            static_cast<std::uint8_t>(length >> 8U),
            static_cast<std::uint8_t>(length & 0xFFU),
            request_header[6]};
        frame.insert(frame.end(), pdu.begin(), pdu.end());
        if (!trickle) {
            static_cast<void>(::send(client_, frame.data(), frame.size(), MSG_NOSIGNAL));
            return;
        }
        for (const std::uint8_t byte : frame) {
            if (stop_ || ::send(client_, &byte, 1, MSG_NOSIGNAL) != 1) {
                return; // the client gave up (closed) or the server is stopping
            }
            std::this_thread::sleep_for(kTrickleDelay);
        }
    }

    int listener_;
    std::uint16_t port_{};
    std::thread thread_;
    int client_{-1}; // server thread only (and the destructor after join)

    std::mutex mutex_; // guards the tables and the mode
    std::vector<std::uint16_t> holding_;
    std::vector<std::uint16_t> input_;
    std::vector<std::uint8_t> coils_;
    std::vector<std::uint8_t> discrete_;
    Mode mode_{Mode::Normal};
    std::uint8_t exception_code_{0x02};

    std::atomic<bool> stop_{false};
    std::atomic<bool> drop_requested_{false};
    std::atomic<int> accepted_{0};
    std::atomic<int> closed_by_peer_{0};
    std::atomic<int> dropped_{0};
    std::atomic<int> requests_{0};
    std::atomic<int> last_unit_id_{-1};
    std::atomic<int> last_function_{-1};
};

} // namespace ieg::modbus::testing
