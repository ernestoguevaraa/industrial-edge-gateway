// End-to-end: the real gateway executable between the real Python OPC UA simulator, a
// deterministic in-process Modbus TCP server and a real Mosquitto broker. Skipped unless both
// IEG_SIMULATOR_PYTHON and IEG_MOSQUITTO are set.

#include "integration/mosquitto_process.hpp"
#include "integration/mqtt_test_subscriber.hpp"
#include "integration/simulator_process.hpp"
#include "modbus/modbus_test_server.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// POSIX process environment for posix_spawn.
// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)
extern char** environ;

namespace ieg::testing {
namespace {

using namespace std::chrono_literals;

struct ExpectedTag
{
    std::string asset;
    std::string tag;
    std::string unit;
};

// topic -> expected metadata, per configs/gateway.example.yaml.
const std::map<std::string, ExpectedTag>& expected_topics()
{
    static const std::map<std::string, ExpectedTag> topics{
        {"industrial/site01/line01/press01/state", {"site01.line01.press01", "state", ""}},
        {"industrial/site01/line01/press01/pressure", {"site01.line01.press01", "pressure", "bar"}},
        {"industrial/site01/line01/press01/cycle_time",
         {"site01.line01.press01", "cycle_time", "s"}},
        {"industrial/site01/line01/press01/production_count",
         {"site01.line01.press01", "production_count", "units"}},
        {"industrial/site01/line01/oven01/state", {"site01.line01.oven01", "state", ""}},
        {"industrial/site01/line01/oven01/temperature",
         {"site01.line01.oven01", "temperature", "degC"}},
        {"industrial/site01/line01/oven01/production_count",
         {"site01.line01.oven01", "production_count", "units"}},
    };
    return topics;
}

std::string gateway_config(std::uint16_t opcua_port, std::uint16_t mqtt_port, int qos)
{
    std::ostringstream yaml;
    yaml << "opcua:\n  endpoint: \"opc.tcp://127.0.0.1:" << opcua_port << "\"\n"
         << "  subscription_interval_ms: 200\n"
         << "mqtt:\n  host: \"127.0.0.1\"\n  port: " << mqtt_port << "\n  qos: " << qos << "\n"
         << "  topic_prefix: \"industrial\"\n  client_id: \"ieg-e2e-gateway\"\n"
         << "runtime:\n  queue_capacity: 1000\n  shutdown_drain_timeout_ms: 2000\n"
         << "assets:\n";
    std::map<std::string, std::vector<std::pair<std::string, ExpectedTag>>> by_asset;
    for (const auto& [topic, tag] : expected_topics()) {
        by_asset[tag.asset].emplace_back(topic, tag);
    }
    const std::map<std::string, std::string> node_names{{"state", "State"},
                                                        {"pressure", "Pressure"},
                                                        {"cycle_time", "CycleTime"},
                                                        {"production_count", "ProductionCount"},
                                                        {"temperature", "Temperature"}};
    for (const auto& [asset, tags] : by_asset) {
        const std::string machine = asset.ends_with("press01") ? "Press01" : "Oven01";
        yaml << "  - asset: \"" << asset << "\"\n    nodes:\n";
        for (const auto& [topic, tag] : tags) {
            yaml << "      - node_id: \"ns=2;s=Line01." << machine << "." << node_names.at(tag.tag)
                 << "\"\n        tag: \"" << tag.tag << "\"\n        unit: \"" << tag.unit
                 << "\"\n";
        }
    }
    return yaml.str();
}

// Modbus device "compressor-plc" served by the in-process test server: topic -> expected JSON
// value.
const std::map<std::string, nlohmann::json>& expected_modbus_values()
{
    static const std::map<std::string, nlohmann::json> values{
        {"industrial/site01/line01/comp01/outlet_pressure", 7.5}, // holding 0 = 75, scale 0.1
        {"industrial/site01/line01/comp01/running", true},        // coil 0
        {"industrial/site01/line01/comp01/motor_current", 2.5},   // input 0..1, float32 ABCD
    };
    return values;
}

void serve_modbus_values(modbus::testing::ModbusTestServer& server, std::uint16_t pressure_raw)
{
    server.set_holding(0, pressure_raw);
    server.set_coil(0, true);
    server.set_input(0, 0x4020); // 2.5f = 0x40200000
    server.set_input(1, 0x0000);
}

std::string modbus_section(std::uint16_t modbus_port)
{
    std::ostringstream yaml;
    yaml << "modbus:\n  devices:\n"
         << "    - source_id: \"compressor-plc\"\n      host: \"127.0.0.1\"\n      port: "
         << modbus_port << "\n      unit_id: 1\n      poll_interval_ms: 100\n"
         << "      timeout_ms: 100\n      points:\n"
         << "        - {asset: \"site01.line01.comp01\", tag: \"outlet_pressure\", unit: \"bar\", "
            "table: holding_registers, offset: 0, type: uint16, scale: 0.1}\n"
         << "        - {asset: \"site01.line01.comp01\", tag: \"running\", unit: \"\", "
            "table: coils, offset: 0, type: bool}\n"
         << "        - {asset: \"site01.line01.comp01\", tag: \"motor_current\", unit: \"A\", "
            "table: input_registers, offset: 0, type: float32, word_order: high_word_first}\n";
    return yaml.str();
}

// The MQTT part of gateway_config() only (Modbus-only deployments have no opcua/assets).
std::string mqtt_only_config(std::uint16_t mqtt_port)
{
    std::ostringstream yaml;
    yaml << "mqtt:\n  host: \"127.0.0.1\"\n  port: " << mqtt_port << "\n  qos: 1\n"
         << "  topic_prefix: \"industrial\"\n  client_id: \"ieg-e2e-gateway\"\n"
         << "runtime:\n  queue_capacity: 1000\n  shutdown_drain_timeout_ms: 2000\n";
    return yaml.str();
}

// A Modbus point payload: the expected value, protocol modbus_tcp, GOOD, no source timestamp.
::testing::AssertionResult valid_modbus_payload(const std::string& topic,
                                                const std::string& payload)
{
    const auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded()) {
        return ::testing::AssertionFailure() << topic << ": payload is not JSON: " << payload;
    }
    if (json.value("protocol", "") != "modbus_tcp" || json.value("quality", "") != "GOOD" ||
        !json.contains("source_timestamp") || !json["source_timestamp"].is_null() ||
        !json["edge_timestamp"].is_string() ||
        json.value("value", nlohmann::json{}) != expected_modbus_values().at(topic)) {
        return ::testing::AssertionFailure() << topic << ": unexpected payload " << payload;
    }
    return ::testing::AssertionSuccess();
}

// Latest payload per topic of `topics`, collected until all arrived or `timeout` passed.
std::map<std::string, std::string> collect_latest_of(MqttTestSubscriber& subscriber,
                                                     const std::set<std::string>& topics,
                                                     std::chrono::milliseconds timeout)
{
    std::map<std::string, std::string> latest;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (latest.size() < topics.size() && std::chrono::steady_clock::now() < deadline) {
        subscriber.wait_for_messages(subscriber.messages().size() + 1, 200ms);
        for (const auto& message : subscriber.messages()) {
            if (topics.contains(message.topic)) {
                latest[message.topic] = message.payload;
            }
        }
    }
    return latest;
}

std::set<std::string> modbus_topics()
{
    std::set<std::string> topics;
    for (const auto& [topic, value] : expected_modbus_values()) {
        topics.insert(topic);
    }
    return topics;
}

// Waits until `topic` carries `value` in a message received after the first `skip` messages.
bool wait_for_value(MqttTestSubscriber& subscriber, std::size_t skip, const std::string& topic,
                    const nlohmann::json& value, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto messages = subscriber.messages();
        for (std::size_t i = skip; i < messages.size(); ++i) {
            if (messages[i].topic != topic) {
                continue;
            }
            const auto json = nlohmann::json::parse(messages[i].payload, nullptr, false);
            if (!json.is_discarded() && json.value("value", nlohmann::json{}) == value) {
                return true;
            }
        }
        subscriber.wait_for_messages(messages.size() + 1, 200ms);
    }
    return false;
}

// Every payload in `latest` satisfies `valid` (topic, payload).
template <typename Validator>
::testing::AssertionResult all_valid(const std::map<std::string, std::string>& latest,
                                     Validator valid)
{
    for (const auto& [topic, payload] : latest) {
        if (auto result = valid(topic, payload); !result) {
            return result;
        }
    }
    return ::testing::AssertionSuccess();
}

// `log` contains every expected fragment.
::testing::AssertionResult log_contains(const std::string& log,
                                        std::initializer_list<const char*> fragments)
{
    for (const char* fragment : fragments) {
        if (log.find(fragment) == std::string::npos) {
            return ::testing::AssertionFailure() << "log lacks '" << fragment << "':\n" << log;
        }
    }
    return ::testing::AssertionSuccess();
}

// No message was published on an OPC UA topic.
::testing::AssertionResult no_opcua_topics(const std::vector<MqttTestSubscriber::Message>& messages)
{
    for (const auto& message : messages) {
        if (expected_topics().contains(message.topic)) {
            return ::testing::AssertionFailure() << "unexpected OPC UA topic " << message.topic;
        }
    }
    return ::testing::AssertionSuccess();
}

// Runs the gateway executable with its output captured to a file.
class GatewayProcess
{
public:
    GatewayProcess(const std::filesystem::path& config, const std::filesystem::path& log)
    {
        std::string binary = IEG_GATEWAY_BINARY;
        std::string config_arg = config.string();
        std::vector<char*> argv{binary.data(), config_arg.data(), nullptr};
        posix_spawn_file_actions_t actions{};
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                           O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        const int rc =
            ::posix_spawn(&pid_, binary.c_str(), &actions, nullptr, argv.data(), environ);
        ::posix_spawn_file_actions_destroy(&actions);
        if (rc != 0) {
            pid_ = -1;
            throw std::runtime_error("could not spawn " + binary);
        }
    }
    ~GatewayProcess()
    {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }
    GatewayProcess(const GatewayProcess&) = delete;
    GatewayProcess& operator=(const GatewayProcess&) = delete;
    GatewayProcess(GatewayProcess&&) = delete;
    GatewayProcess& operator=(GatewayProcess&&) = delete;

    void signal(int signum) const { ::kill(pid_, signum); }

    // Exit code if the process exited normally within `timeout`.
    std::optional<int> wait_exit(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? std::optional<int>{WEXITSTATUS(status)} : std::nullopt;
            }
            std::this_thread::sleep_for(20ms);
        }
        return std::nullopt;
    }

private:
    pid_t pid_{-1};
};

std::string read_file(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return {};
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    std::ifstream file{path, std::ios::binary};
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(file.gcount()));
    return content;
}

// Latest payload per expected topic, collected until all arrived or `timeout` passed.
std::map<std::string, std::string> collect_latest(MqttTestSubscriber& subscriber,
                                                  std::chrono::milliseconds timeout)
{
    std::map<std::string, std::string> latest;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (latest.size() < expected_topics().size() &&
           std::chrono::steady_clock::now() < deadline) {
        subscriber.wait_for_messages(latest.size() + 1, 500ms);
        for (const auto& message : subscriber.messages()) {
            if (expected_topics().contains(message.topic)) {
                latest[message.topic] = message.payload;
            }
        }
    }
    return latest;
}

::testing::AssertionResult valid_payload(const std::string& topic, const std::string& payload)
{
    const auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded()) {
        return ::testing::AssertionFailure() << topic << ": payload is not JSON: " << payload;
    }
    const auto& expected = expected_topics().at(topic);
    for (const char* key : {"asset", "tag", "value", "unit", "quality", "source_timestamp",
                            "edge_timestamp", "protocol"}) {
        if (!json.contains(key)) {
            return ::testing::AssertionFailure() << topic << ": missing '" << key << "'";
        }
    }
    const std::set<std::string> qualities{"GOOD", "UNCERTAIN", "BAD"};
    if (json["asset"] != expected.asset || json["tag"] != expected.tag ||
        json["unit"] != expected.unit || json["protocol"] != "opcua" ||
        !qualities.contains(json["quality"].get<std::string>()) ||
        !json["source_timestamp"].is_string() || !json["edge_timestamp"].is_string() ||
        json["edge_timestamp"].get<std::string>() < json["source_timestamp"].get<std::string>()) {
        return ::testing::AssertionFailure() << topic << ": unexpected payload " << payload;
    }
    return ::testing::AssertionSuccess();
}

class GatewayEndToEnd : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto python = SimulatorProcess::python_from_environment();
        const auto mosquitto = MosquittoProcess::binary_from_environment();
        if (!python || !mosquitto) {
            GTEST_SKIP() << "IEG_SIMULATOR_PYTHON and IEG_MOSQUITTO are required";
        }
        simulator_ = std::make_unique<SimulatorProcess>(*python, SimulatorProcess::free_port());
        simulator_->launch();
        broker_ = std::make_unique<MosquittoProcess>(*mosquitto, SimulatorProcess::free_port());
        broker_->launch();
        directory_ = std::filesystem::temp_directory_path() /
                     ("ieg-e2e-" + std::to_string(::getpid()) + "-" +
                      ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path write_config(int qos)
    {
        return write_file(gateway_config(opcua_port(), broker_->port(), qos));
    }

    std::filesystem::path write_mixed_config(std::uint16_t modbus_port)
    {
        return write_file(gateway_config(opcua_port(), broker_->port(), 1) +
                          modbus_section(modbus_port));
    }

    std::filesystem::path write_modbus_only_config(std::uint16_t modbus_port)
    {
        return write_file(mqtt_only_config(broker_->port()) + modbus_section(modbus_port));
    }

    std::filesystem::path write_file(const std::string& yaml)
    {
        const auto path = directory_ / "gateway.yaml";
        std::ofstream{path} << yaml;
        return path;
    }

    [[nodiscard]] std::uint16_t opcua_port() const
    {
        const auto endpoint = simulator_->endpoint();
        return static_cast<std::uint16_t>(std::stoi(endpoint.substr(endpoint.rfind(':') + 1)));
    }

    [[nodiscard]] std::string broker_uri() const
    {
        return "tcp://127.0.0.1:" + std::to_string(broker_->port());
    }

    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    std::unique_ptr<SimulatorProcess> simulator_;
    std::unique_ptr<MosquittoProcess> broker_;
};

TEST_F(GatewayEndToEnd, PublishesEveryConfiguredTagAndShutsDownCleanlyOnSigterm)
{
    MqttTestSubscriber subscriber{broker_uri(), "ieg-e2e-subscriber"};
    subscriber.subscribe("industrial/#");

    const auto log = directory() / "gateway.log";
    GatewayProcess gateway{write_config(1), log};

    // Every configured topic must arrive with a valid payload.
    const auto latest = collect_latest(subscriber, 30s);
    ASSERT_EQ(latest.size(), expected_topics().size()) << read_file(log);
    for (const auto& [topic, payload] : latest) {
        EXPECT_TRUE(valid_payload(topic, payload));
    }

    gateway.signal(SIGTERM);
    EXPECT_EQ(gateway.wait_exit(10s), std::optional<int>{0})
        << "gateway did not exit with code 0 within 10 s\n"
        << read_file(log);

    const auto output = read_file(log);
    for (const char* expected :
         {"event=started", "component=opcua", "component=mqtt", "state=Connected",
          "event=shutdown_requested signal=SIGTERM", "event=stopping", "event=stopped"}) {
        EXPECT_NE(output.find(expected), std::string::npos) << expected << "\n" << output;
    }
}

TEST_F(GatewayEndToEnd, QosOtherThan1FailsAtStartupWithExitCode2)
{
    const auto log = directory() / "gateway.log";
    GatewayProcess gateway{write_config(0), log};
    EXPECT_EQ(gateway.wait_exit(10s), std::optional<int>{2});
    EXPECT_NE(read_file(log).find("mqtt.qos must be 1"), std::string::npos) << read_file(log);
}

TEST_F(GatewayEndToEnd, OpcUaAndModbusPublishThroughOneBrokerAndModbusRecovers)
{
    modbus::testing::ModbusTestServer modbus_server;
    serve_modbus_values(modbus_server, 75);
    MqttTestSubscriber subscriber{broker_uri(), "ieg-e2e-subscriber"};
    subscriber.subscribe("industrial/#");

    const auto log = directory() / "gateway.log";
    GatewayProcess gateway{write_mixed_config(modbus_server.port()), log};

    // MVP 1 OPC UA topics and payloads are unchanged next to the Modbus points.
    const auto opcua = collect_latest(subscriber, 30s);
    ASSERT_EQ(opcua.size(), expected_topics().size()) << read_file(log);
    EXPECT_TRUE(all_valid(opcua, valid_payload));
    const auto modbus = collect_latest_of(subscriber, modbus_topics(), 15s);
    ASSERT_EQ(modbus.size(), expected_modbus_values().size()) << read_file(log);
    EXPECT_TRUE(all_valid(modbus, valid_modbus_payload));

    // Modbus outage: the server stops answering, then answers again with a new value. The same
    // gateway process must time out, reconnect and publish the fresh value.
    modbus_server.set_mode(modbus::testing::ModbusTestServer::Mode::Silent);
    std::this_thread::sleep_for(500ms);
    modbus_server.set_holding(0, 123); // 12.3 bar
    const auto before = subscriber.messages().size();
    modbus_server.set_mode(modbus::testing::ModbusTestServer::Mode::Normal);
    EXPECT_TRUE(wait_for_value(subscriber, before,
                               "industrial/site01/line01/comp01/outlet_pressure", 12.3, 20s))
        << read_file(log);
    EXPECT_GE(modbus_server.accepted(), 2);
    EXPECT_EQ(modbus_server.last_unit_id(), 1);

    gateway.signal(SIGTERM);
    EXPECT_EQ(gateway.wait_exit(10s), std::optional<int>{0}) << read_file(log);
    EXPECT_TRUE(log_contains(read_file(log),
                             {"event=started sources=2", "component=opcua source_id=opcua-main",
                              "component=modbus_tcp source_id=compressor-plc", "state=Disconnected",
                              "detail=\"timeout: ", "event=stopped"}));
}

TEST_F(GatewayEndToEnd, ModbusOnlyConfigurationPublishesAndShutsDownCleanly)
{
    modbus::testing::ModbusTestServer modbus_server;
    serve_modbus_values(modbus_server, 75);
    MqttTestSubscriber subscriber{broker_uri(), "ieg-e2e-subscriber"};
    subscriber.subscribe("industrial/#");

    const auto log = directory() / "gateway.log";
    GatewayProcess gateway{write_modbus_only_config(modbus_server.port()), log};
    const auto modbus = collect_latest_of(subscriber, modbus_topics(), 15s);
    ASSERT_EQ(modbus.size(), expected_modbus_values().size()) << read_file(log);
    EXPECT_TRUE(all_valid(modbus, valid_modbus_payload));
    EXPECT_TRUE(no_opcua_topics(subscriber.messages()));
    gateway.signal(SIGTERM);
    EXPECT_EQ(gateway.wait_exit(10s), std::optional<int>{0}) << read_file(log);
    EXPECT_TRUE(log_contains(read_file(log), {"event=started sources=1"}));
}

} // namespace
} // namespace ieg::testing
