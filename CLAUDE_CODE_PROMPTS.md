# Claude Code Prompts — MVP 1

Use one issue at a time. Review every generated change before accepting it.

## Prompt for Issue #1

Implement Issue #1 from `docs/ISSUES.md`.

Constraints:

- Use C++20.
- Use CMake.
- Create a `gateway` executable.
- Add GoogleTest, spdlog, yaml-cpp and nlohmann/json.
- Enable strict compiler warnings for GCC/Clang.
- Add `.clang-format`.
- Add a reasonable `.clang-tidy`.
- Add a GitHub Actions workflow for configure/build/test.
- Add one placeholder unit test.
- Do not implement OPC UA.
- Do not implement MQTT.
- Do not add unnecessary frameworks or abstractions.

Before editing files:
1. summarize the proposed file changes;
2. identify any dependency acquisition strategy;
3. wait for approval if the dependency strategy changes the repository architecture.

After implementation:
1. run configure/build/tests;
2. report the exact commands;
3. report warnings or failures;
4. summarize all changed files.

## Prompt for Issue #2

Implement Issue #2 from `docs/ISSUES.md`.

Read:
- `docs/ARCHITECTURE.md`
- `docs/adr/002-decouple-protocols-from-domain.md`

Create a protocol-neutral domain model for normalized industrial telemetry.

Constraints:

- No OPC UA headers.
- No MQTT headers.
- Strongly typed `DataQuality`.
- `TelemetryValue` must support bool, signed integer, unsigned integer, double and string.
- Add JSON serialization with nlohmann/json.
- Add unit tests.
- Keep the model small and explicit.
- Do not introduce inheritance unless clearly justified.

Before editing:
- propose the types and namespaces.

After implementation:
- build and run tests;
- show example serialized JSON.

## Prompt for Issue #3

Implement Issue #3 from `docs/ISSUES.md`.

Use `configs/gateway.example.yaml` as the intended configuration shape.

Requirements:
- load YAML;
- validate mandatory fields;
- reject duplicated asset/tag pairs;
- reject invalid MQTT QoS;
- produce actionable error messages;
- add tests for valid and invalid configurations.

Do not add OPC UA or MQTT runtime code yet.

## Prompt for Issue #4

Implement Issue #4 from `docs/ISSUES.md`.

Create a Python OPC UA industrial plant simulator using `asyncua`.

Requirements:
- endpoint port 4840;
- expose the documented Press01 and Oven01 nodes;
- maintain equipment state;
- counters only increment while equipment is running;
- pressure/temperature should change smoothly within credible ranges rather than pure random values;
- source timestamps must be generated;
- Dockerfile included;
- startup logs should list endpoint and namespace.

Do not implement the C++ client in this issue.
