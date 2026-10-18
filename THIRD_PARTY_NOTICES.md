# Third-Party Notices

The Industrial Edge Gateway is licensed under the Apache License 2.0 (see [LICENSE](LICENSE)).
It uses the third-party components below, each under its own license. This file is
informational and not legal advice; the license texts distributed with each component govern.

| Component | Version | Role in this project | License |
|---|---|---|---|
| [open62541](https://github.com/open62541/open62541) | 1.4.21 (official amalgamation, unmodified) | OPC UA client, statically linked into the gateway | MPL-2.0 |
| [Eclipse Paho MQTT C](https://github.com/eclipse-paho/paho.mqtt.c) | 1.3.16 | MQTT 5 client (`MQTTAsync`), statically linked into the gateway | EPL-2.0 / EDL-1.0 (dual) |
| [spdlog](https://github.com/gabime/spdlog) | 1.15.0 | Logging | MIT |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | YAML configuration parsing | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | JSON payload serialization | MIT |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 2.28.x (Ubuntu 24.04 package, 2.28.8) | Required by the open62541 amalgamation; dynamically linked system library | Apache-2.0 OR GPL-2.0-or-later |
| [GoogleTest / GoogleMock](https://github.com/google/googletest) | 1.15.2 | Unit and integration tests only; not part of the gateway executable | BSD-3-Clause |
| [asyncua](https://github.com/FreeOpcUa/opcua-asyncio) | 1.1.8 | OPC UA server of the Python plant simulator | LGPL-3.0-or-later |
| [Eclipse Mosquitto](https://github.com/eclipse-mosquitto/mosquitto) | 2.0.22 (`eclipse-mosquitto` image in the Compose demo); Ubuntu package for integration tests | MQTT broker for the demo and the integration tests; not part of the gateway | EPL-2.0 / EDL-1.0 (dual) |

Versions are the ones pinned in [`cmake/Dependencies.cmake`](cmake/Dependencies.cmake) (by
SHA256), [`simulator/requirements.txt`](simulator/requirements.txt) and
[`compose.yaml`](compose.yaml); Mbed TLS and the integration-test Mosquitto come from the Ubuntu
24.04 packages used by the build environment and CI.

Additional transitive components are not listed here and are governed by their own licenses:

- the Python packages that asyncua depends on (pinned in `simulator/requirements.txt`) and the
  simulator's development tools;
- the Docker base images (`ubuntu:24.04`, `python:3.12-slim`, `eclipse-mosquitto`) and the
  operating-system packages they contain.

This file is not a software bill of materials.
