# MVP 1 Implementation Backlog

Each issue is intentionally scoped so it can be implemented and reviewed independently, including when using an AI coding agent.

## #1 Bootstrap C++20 project

### Goal
Create a production-quality CMake project.

### Requirements
- C++20
- gateway executable
- GoogleTest
- spdlog
- yaml-cpp
- nlohmann/json
- strict compiler warnings
- clang-format
- clang-tidy
- `ctest`
- GitHub Actions build/test workflow

### Acceptance criteria
- clean configure/build from empty `build/`
- one placeholder unit test passes
- CI runs on push and pull request

---

## #2 Define domain telemetry model

### Goal
Create protocol-independent telemetry types.

### Requirements
- `TelemetryValue`
- `TelemetryPoint`
- `DataQuality`
- timestamp representation
- JSON serialization
- unit tests

### Acceptance criteria
- supports bool/integer/unsigned/double/string
- serialization output is deterministic
- no OPC UA or MQTT dependency in domain model

---

## #3 Implement configuration model

### Goal
Load gateway configuration from YAML.

### Requirements
- OPC UA endpoint
- MQTT host/port/qos
- asset mappings
- node IDs
- tag/unit metadata
- validation

### Acceptance criteria
- valid config loads
- malformed config fails clearly
- duplicated asset/tag fails validation
- unit tests cover validation

---

## #4 Build Python OPC UA plant simulator

### Goal
Expose realistic OPC UA nodes for Line01.

### Requirements
- Python `asyncua`
- Press01
- Oven01
- deterministic/stateful signal generation
- source timestamps
- OPC UA quality
- Dockerfile

### Acceptance criteria
- endpoint reachable on port 4840
- all documented NodeIds can be browsed/read
- counters behave consistently with equipment state

---

## #5 Implement `IOpcUaClient`

### Goal
Define protocol abstraction used by gateway core.

### Requirements
- connect
- disconnect
- subscribe
- lifecycle/status callback if required
- no `open62541` types outside adapter boundary

### Acceptance criteria
- interface has no MQTT dependency
- interface can be mocked in tests

---

## #6 Implement `Open62541Client`

### Goal
Connect to simulator and subscribe to configured nodes.

### Requirements
- `open62541`
- monitored items
- value conversion
- source timestamps
- quality mapping
- reconnect handling

### Acceptance criteria
- receives configured simulator changes
- maps values to protocol-neutral representation
- reconnects after simulator restart

---

## #7 Implement bounded telemetry queue

### Goal
Decouple OPC UA callbacks from MQTT network operations.

### Requirements
- bounded capacity
- thread-safe
- clear overflow policy
- clean shutdown

### Acceptance criteria
- OPC UA callback never performs MQTT publish
- queue behavior covered by tests
- shutdown cannot deadlock

---

## #8 Implement MQTT publisher abstraction

### Goal
Define protocol-independent output interface.

### Requirements
- publish TelemetryPoint
- connect/disconnect lifecycle
- error result semantics

### Acceptance criteria
- gateway core depends on interface, not MQTT library

---

## #9 Implement MQTT 5 publisher

### Goal
Publish normalized telemetry to Mosquitto.

### Requirements
- MQTT 5
- QoS 1
- deterministic topic generation
- JSON payload
- reconnect handling

### Acceptance criteria
- `mosquitto_sub` receives expected messages
- MQTT restart is recovered automatically

---

## #10 Compose gateway runtime

### Goal
Wire configuration, OPC UA adapter, queue and MQTT publisher.

### Requirements
- explicit ownership
- signal handling
- graceful shutdown
- startup validation
- component-level logs

### Acceptance criteria
- SIGINT/SIGTERM shutdown cleanly
- no orphan worker threads
- clear startup and connectivity logs

---

## #11 Add Docker Compose demo

### Goal
Run all MVP services with one command.

### Services
- plant-simulator
- mosquitto
- edge-gateway

### Acceptance criteria
```bash
docker compose up --build
```

starts a functioning data path.

---

## #12 Add integration test

### Goal
Verify end-to-end telemetry.

### Acceptance criteria
Automated test verifies:
- expected MQTT topic appears
- JSON payload is valid
- asset/tag match configuration
- source timestamp exists
- edge timestamp exists
- quality is valid

---

## #13 Add connectivity recovery test

### Goal
Demonstrate basic resilience.

### Cases
- OPC UA simulator restart
- Mosquitto restart

### Acceptance criteria
Gateway remains running and automatically reconnects.

---

## #14 Finalize portfolio documentation

### Goal
Make MVP 1 understandable in under three minutes.

### Requirements
- architecture diagram
- one-command demo
- example output
- design rationale
- known limitations
- roadmap

### Acceptance criteria
A reviewer can understand:
1. what the gateway does
2. why the architecture matters
3. how to run it
4. what is intentionally deferred
