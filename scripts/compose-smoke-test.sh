#!/usr/bin/env bash
# Smoke test of the MVP 1 Compose demo (compose.yaml).
#
# Host prerequisites: Docker (with the Compose plugin) and Bash only. JSON is parsed with the
# Python interpreter inside the plant-simulator container.
#
# Runs as its own Compose project with random loopback host ports, so it does not interfere with
# a running demo. Verifies that:
#   1. the exact set of seven configured MQTT topics arrives, every payload is valid JSON with
#      the required fields and the expected asset/tag/unit;
#   2. the gateway logs its start and both connections;
#   3. the gateway container stops on SIGTERM with exit code 0 within the grace period.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

readonly PROJECT="${IEG_SMOKE_PROJECT:-ieg-smoke}"
readonly TOPIC_TIMEOUT_S=90 # safety bound for the subscriber; it is stopped explicitly
readonly STOP_GRACE_S=15

export IEG_MQTT_HOST_PORT=0  # random loopback ports
export IEG_OPCUA_HOST_PORT=0

compose() { docker compose -p "$PROJECT" "$@"; }

work="$(mktemp -d)"
sub_pid=""

stop_subscriber() {
    if [[ -n "$sub_pid" ]]; then
        # Terminate mosquitto_sub inside the container, then reap the host-side exec client.
        compose exec -T mosquitto pkill -x mosquitto_sub >/dev/null 2>&1 || true
        kill "$sub_pid" >/dev/null 2>&1 || true
        wait "$sub_pid" 2>/dev/null || true
        sub_pid=""
    fi
}

cleanup() {
    local status=$?
    stop_subscriber
    if ((status != 0)); then
        echo "---- diagnostics (project $PROJECT) ----" >&2
        compose ps -a >&2 || true
        compose logs --no-color --tail=60 >&2 || true
    fi
    compose down --remove-orphans >/dev/null 2>&1 || true
    rm -rf "$work"
    exit "$status"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

# Topic -> "asset|tag|unit", per configs/gateway.example.yaml.
readonly EXPECTED=(
    "industrial/site01/line01/press01/state|site01.line01.press01|state|"
    "industrial/site01/line01/press01/pressure|site01.line01.press01|pressure|bar"
    "industrial/site01/line01/press01/cycle_time|site01.line01.press01|cycle_time|s"
    "industrial/site01/line01/press01/production_count|site01.line01.press01|production_count|units"
    "industrial/site01/line01/oven01/state|site01.line01.oven01|state|"
    "industrial/site01/line01/oven01/temperature|site01.line01.oven01|temperature|degC"
    "industrial/site01/line01/oven01/production_count|site01.line01.oven01|production_count|units"
)

# Reads "topic payload" lines on stdin. Fails unless the set of industrial/ topics equals the
# expected set exactly and every payload parses as a JSON object with the required fields.
readonly VALIDATOR='
import json, sys
expected = {}
for spec in sys.argv[1:]:
    topic, asset, tag, unit = spec.split("|")
    expected[topic] = (asset, tag, unit)
required = ["asset", "tag", "value", "unit", "quality", "source_timestamp", "edge_timestamp",
            "protocol"]
seen, errors, count = set(), [], 0
for line in sys.stdin:
    line = line.rstrip("\n")
    if not line.startswith("industrial/"):
        continue
    topic, _, payload = line.partition(" ")
    seen.add(topic)
    count += 1
    if topic not in expected:
        continue
    try:
        data = json.loads(payload)
    except json.JSONDecodeError as error:
        errors.append(f"{topic}: invalid JSON ({error}): {payload}")
        continue
    if not isinstance(data, dict):
        errors.append(f"{topic}: payload is not a JSON object")
        continue
    missing = [key for key in required if key not in data]
    if missing:
        errors.append(f"{topic}: missing fields {missing}")
        continue
    asset, tag, unit = expected[topic]
    if (data["asset"], data["tag"], data["unit"]) != (asset, tag, unit):
        errors.append(f"{topic}: unexpected asset/tag/unit {data['asset']}/{data['tag']}/{data['unit']}")
    if data["protocol"] != "opcua":
        errors.append(f"{topic}: protocol {data['protocol']!r}")
    if data["quality"] not in ("GOOD", "UNCERTAIN", "BAD"):
        errors.append(f"{topic}: quality {data['quality']!r}")
    if not isinstance(data["edge_timestamp"], str) or not data["edge_timestamp"].endswith("Z"):
        errors.append(f"{topic}: edge_timestamp {data['edge_timestamp']!r}")
    if data["source_timestamp"] is not None and not isinstance(data["source_timestamp"], str):
        errors.append(f"{topic}: source_timestamp {data['source_timestamp']!r}")
if seen != set(expected):
    errors.append(f"topic set mismatch: unexpected {sorted(seen - set(expected))}, "
                  f"missing {sorted(set(expected) - seen)}")
for error in errors:
    print("  " + error)
print(f"validated {count} messages on {len(seen)} topics")
sys.exit(1 if errors else 0)
'

topics_seen() {
    local missing=0 entry
    for entry in "${EXPECTED[@]}"; do
        grep -q "^${entry%%|*} " "$work/messages.txt" || missing=$((missing + 1))
    done
    return "$missing"
}

echo "==> building images"
compose build

echo "==> starting mosquitto and plant-simulator"
compose up -d --wait mosquitto plant-simulator

echo "==> subscribing to industrial/# (before the gateway starts, to capture initial values)"
compose exec -T mosquitto mosquitto_sub -t 'industrial/#' -t 'smoke/probe' -v \
    -W "$TOPIC_TIMEOUT_S" >"$work/messages.txt" 2>"$work/subscriber.err" &
sub_pid=$!
# Confirm the subscription is active with a probe outside industrial/ before starting the gateway.
for _ in $(seq 1 50); do
    grep -q '^smoke/probe ' "$work/messages.txt" && break
    compose exec -T mosquitto mosquitto_pub -t smoke/probe -m ready >/dev/null 2>&1 || true
    sleep 0.2
done
grep -q '^smoke/probe ' "$work/messages.txt" || fail "subscriber did not become ready"

echo "==> starting edge-gateway"
compose up -d edge-gateway

echo "==> waiting for the ${#EXPECTED[@]} expected topics"
deadline=$((SECONDS + TOPIC_TIMEOUT_S - 10))
until topics_seen; do
    ((SECONDS < deadline)) || fail "not all expected topics arrived within the time bound"
    kill -0 "$sub_pid" 2>/dev/null || fail "subscriber exited early: $(cat "$work/subscriber.err")"
    sleep 1
done
stop_subscriber

echo "==> validating payloads (JSON parsed inside the plant-simulator container)"
compose exec -T plant-simulator python -c "$VALIDATOR" "${EXPECTED[@]}" <"$work/messages.txt" ||
    fail "payload validation failed"

echo "==> checking gateway logs"
gateway_logs="$(compose logs --no-color edge-gateway)"
for pattern in 'event=started' 'component=opcua .*state=Connected' 'component=mqtt .*state=Connected'; do
    grep -Eq "$pattern" <<<"$gateway_logs" || fail "gateway log lacks '$pattern'"
done

echo "==> stopping edge-gateway with SIGTERM"
container="$(compose ps -q edge-gateway)"
start=$SECONDS
compose stop -t "$STOP_GRACE_S" edge-gateway >/dev/null
elapsed=$((SECONDS - start))
exit_code="$(docker inspect -f '{{.State.ExitCode}}' "$container")"
gateway_logs="$(compose logs --no-color edge-gateway)"
((exit_code == 0)) || fail "gateway exit code $exit_code (137 means it was killed after the grace period)"
((elapsed < STOP_GRACE_S)) || fail "gateway took ${elapsed}s to stop"
grep -q 'event=shutdown_requested signal=SIGTERM' <<<"$gateway_logs" || fail "no SIGTERM shutdown log"
grep -q 'event=stopped' <<<"$gateway_logs" || fail "no 'event=stopped' log"
echo "    exit code 0 after ${elapsed}s"

echo "PASS: Compose demo smoke test"
