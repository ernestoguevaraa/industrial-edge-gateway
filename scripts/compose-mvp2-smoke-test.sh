#!/usr/bin/env bash
# Smoke test of the MVP 2 mixed Compose demo (compose.mvp2.yaml): OPC UA plant simulator and
# Modbus TCP compressor simulator through one gateway and one Mosquitto broker.
#
# Host prerequisites: Docker (with the Compose plugin) and Bash only. JSON is parsed with the
# Python interpreter inside the plant-simulator container.
#
# Runs as its own Compose project with random loopback host ports, so it does not interfere with
# a running demo. Verifies that:
#   1. the gateway starts both sources (log) and connects to OPC UA, Modbus TCP and MQTT;
#   2. exactly the 7 OPC UA and 11 Modbus topics arrive (no unexpected topic, so no collision
#      between sources), every payload has the expected asset/tag/unit and protocol
#      ("opcua" / "modbus_tcp"), Modbus values, scales and JSON types match the simulator's
#      documented register map, and Modbus payloads have "source_timestamp": null;
#   3. after a restart of only the Modbus simulator, the same gateway process reconnects and
#      publishes fresh Modbus data, while OPC UA publishing continues without a gap;
#   4. after a Mosquitto restart, fresh OPC UA and Modbus telemetry flows again;
#   5. the gateway container stops on SIGTERM with exit code 0 within the grace period.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

readonly PROJECT="${IEG_SMOKE_PROJECT:-ieg-smoke-mvp2}"
readonly TOPIC_TIMEOUT_S=90 # safety bound for the subscriber; it is stopped explicitly
readonly STOP_GRACE_S=15
readonly RECOVERY_TIMEOUT_S=60 # bound for reconnect + fresh telemetry after each restart
readonly MAX_OPCUA_GAP_S=5     # oven temperature changes every simulator tick (0.5 s)

export IEG_MQTT_HOST_PORT=0 # random loopback ports
export IEG_OPCUA_HOST_PORT=0
export IEG_MODBUS_HOST_PORT=0

compose() { docker compose -f compose.mvp2.yaml -p "$PROJECT" "$@"; }

work="$(mktemp -d)"
sub_pid=""

stop_subscriber() {
    if [[ -n "$sub_pid" ]]; then
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

# Starts mosquitto_sub on industrial/# writing to $1 and returns once it is subscribed, proven
# by receiving a probe published outside industrial/ (retried, e.g. while the broker restarts).
start_subscriber() {
    local file="$1"
    : >"$file"
    compose exec -T mosquitto mosquitto_sub -t 'industrial/#' -t 'smoke/probe' -v \
        -W "$TOPIC_TIMEOUT_S" >"$file" 2>"$work/subscriber.err" &
    sub_pid=$!
    for _ in $(seq 1 100); do
        grep -q '^smoke/probe ' "$file" && return 0
        if ! kill -0 "$sub_pid" 2>/dev/null; then # broker not accepting yet: start again
            wait "$sub_pid" 2>/dev/null || true
            compose exec -T mosquitto mosquitto_sub -t 'industrial/#' -t 'smoke/probe' -v \
                -W "$TOPIC_TIMEOUT_S" >"$file" 2>"$work/subscriber.err" &
            sub_pid=$!
        fi
        compose exec -T mosquitto mosquitto_pub -t smoke/probe -m ready >/dev/null 2>&1 || true
        sleep 0.2
    done
    fail "subscriber did not become ready"
}

gateway_id=""
gateway_started_at=""
assert_same_gateway_running() {
    local id state
    id="$(compose ps -q edge-gateway)"
    [[ "$id" == "$gateway_id" ]] || fail "edge-gateway container changed ($gateway_id -> $id)"
    state="$(docker inspect -f '{{.State.Running}} {{.State.StartedAt}} {{.RestartCount}}' "$id")"
    [[ "$state" == "true $gateway_started_at 0" ]] ||
        fail "edge-gateway is not the same running process (state: $state)"
}

utc_now() { date -u +%Y-%m-%dT%H:%M:%S.%3NZ; }

pyrun() { compose exec -T plant-simulator python -c "$@"; }

readonly OPCUA_TOPICS=(
    industrial/site01/line01/press01/state
    industrial/site01/line01/press01/pressure
    industrial/site01/line01/press01/cycle_time
    industrial/site01/line01/press01/production_count
    industrial/site01/line01/oven01/state
    industrial/site01/line01/oven01/temperature
    industrial/site01/line01/oven01/production_count
)
readonly MODBUS_PREFIX=industrial/site01/line01/compressor01
readonly MODBUS_TAGS=(running maintenance_mode door_closed low_oil outlet_pressure
    suction_temperature running_time motor_current pressure_delta oil_temperature cycle_count)
readonly OPCUA_CONTINUOUS=industrial/site01/line01/oven01/temperature
readonly MODBUS_CONTINUOUS=("$MODBUS_PREFIX/cycle_count" "$MODBUS_PREFIX/outlet_pressure")

# Reads "topic payload" lines on stdin. Fails unless the industrial/ topics are exactly the 18
# expected ones and every payload matches its source's contract (configs/gateway.example.yaml,
# configs/gateway.mixed.example.yaml, modbus-simulator/README.md).
readonly VALIDATOR='
import json, math, sys
OPCUA = {
    "industrial/site01/line01/press01/state": ("site01.line01.press01", "state", ""),
    "industrial/site01/line01/press01/pressure": ("site01.line01.press01", "pressure", "bar"),
    "industrial/site01/line01/press01/cycle_time": ("site01.line01.press01", "cycle_time", "s"),
    "industrial/site01/line01/press01/production_count":
        ("site01.line01.press01", "production_count", "units"),
    "industrial/site01/line01/oven01/state": ("site01.line01.oven01", "state", ""),
    "industrial/site01/line01/oven01/temperature": ("site01.line01.oven01", "temperature", "degC"),
    "industrial/site01/line01/oven01/production_count":
        ("site01.line01.oven01", "production_count", "units"),
}
def is_int(v): return isinstance(v, int) and not isinstance(v, bool)
def is_float(v): return isinstance(v, float)
def close(v, x): return is_float(v) and math.isclose(v, x, abs_tol=1e-9)
def pressure(v): return is_float(v) and (close(v, 0.0) or any(close(v, 70 + 0.5 * i) for i in range(10)))
# tag -> (unit, value check, rule)
MODBUS = {
    "running": ("", lambda v: isinstance(v, bool), "bool"),
    "maintenance_mode": ("", lambda v: v is False, "false"),
    "door_closed": ("", lambda v: v is True, "true"),
    "low_oil": ("", lambda v: v is False, "false"),
    "outlet_pressure": ("bar", pressure, "float 0.0 or 70.0..74.5 step 0.5 (uint16 x0.1)"),
    "suction_temperature": ("degC", lambda v: close(v, -12.5), "float -12.5 (int16 x0.1)"),
    "running_time": ("s", lambda v: is_int(v) and v >= 100000, "integer >= 100000 (uint32)"),
    "motor_current": ("A", lambda v: close(v, 12.5) or close(v, 0.0), "float 12.5 or 0.0"),
    "pressure_delta": ("Pa", lambda v: v == -3500 and is_int(v), "integer -3500 (int32 CDAB)"),
    "oil_temperature": ("degC", lambda v: close(v, 48.25), "float 48.25 (float32 CDAB)"),
    "cycle_count": ("", lambda v: is_int(v) and 0 <= v <= 65535, "integer 0..65535 (uint16)"),
}
PREFIX = "industrial/site01/line01/compressor01/"
expected = set(OPCUA) | {PREFIX + tag for tag in MODBUS}
required = ["asset", "tag", "value", "unit", "quality", "source_timestamp", "edge_timestamp",
            "protocol"]
seen, errors, counts = set(), [], {"opcua": 0, "modbus_tcp": 0}
for line in sys.stdin:
    line = line.rstrip("\n")
    if not line.startswith("industrial/"):
        continue
    topic, _, payload = line.partition(" ")
    seen.add(topic)
    if topic not in expected:
        continue
    try:
        data = json.loads(payload)
    except json.JSONDecodeError as error:
        errors.append(f"{topic}: invalid JSON ({error})")
        continue
    missing = [key for key in required if key not in data]
    if missing:
        errors.append(f"{topic}: missing fields {missing}")
        continue
    if not isinstance(data["edge_timestamp"], str) or not data["edge_timestamp"].endswith("Z"):
        errors.append(f"{topic}: edge_timestamp {data["edge_timestamp"]!r}")
    if topic in OPCUA:
        asset, tag, unit = OPCUA[topic]
        if data["protocol"] != "opcua":
            errors.append(f"{topic}: protocol {data["protocol"]!r}, expected opcua")
        if data["quality"] not in ("GOOD", "UNCERTAIN", "BAD"):
            errors.append(f"{topic}: quality {data["quality"]!r}")
        if not isinstance(data["source_timestamp"], (str, type(None))):
            errors.append(f"{topic}: source_timestamp {data["source_timestamp"]!r}")
    else:
        tag = topic[len(PREFIX):]
        asset, (unit, check, rule) = "site01.line01.compressor01", MODBUS[tag]
        if data["protocol"] != "modbus_tcp":
            errors.append(f"{topic}: protocol {data["protocol"]!r}, expected modbus_tcp")
        if data["quality"] != "GOOD":
            errors.append(f"{topic}: quality {data["quality"]!r}")
        if data["source_timestamp"] is not None:
            errors.append(f"{topic}: source_timestamp {data["source_timestamp"]!r}, expected null")
        if not check(data["value"]):
            errors.append(f"{topic}: value {data["value"]!r} violates: {rule}")
    if (data["asset"], data["tag"], data["unit"]) != (asset, tag, unit):
        errors.append(f"{topic}: asset/tag/unit {data["asset"]}/{data["tag"]}/{data["unit"]}")
    counts[data["protocol"]] = counts.get(data["protocol"], 0) + 1
if seen != expected:
    errors.append(f"topic set mismatch: unexpected {sorted(seen - expected)}, "
                  f"missing {sorted(expected - seen)}")
for error in errors[:40]:
    print("  " + error)
print(f"validated {sum(counts.values())} messages on {len(seen)} topics "
      f"(opcua={counts.get("opcua", 0)}, modbus_tcp={counts.get("modbus_tcp", 0)})")
sys.exit(1 if errors else 0)
'

# argv: since topic...; stdin "topic payload". Succeeds once every topic has a message whose
# edge_timestamp is later than `since`; prints the recovery time to the first fresh message.
readonly FRESHNESS_CHECKER='
import json, sys
from datetime import datetime
def ts(text): return datetime.strptime(text, "%Y-%m-%dT%H:%M:%S.%fZ")
since, topics = sys.argv[1], sys.argv[2:]
first = {}
for line in sys.stdin:
    topic, _, payload = line.rstrip("\n").partition(" ")
    if topic not in topics:
        continue
    try:
        stamp = json.loads(payload)["edge_timestamp"]
    except (json.JSONDecodeError, KeyError, TypeError):
        print(f"  invalid payload on {topic}: {payload}")
        sys.exit(2)
    if stamp > since and topic not in first:
        first[topic] = stamp
if len(first) < len(topics):
    sys.exit(1)
recovery = max((ts(stamp) - ts(since)).total_seconds() for stamp in first.values())
print(f"  first fresh message on every topic {recovery:.1f}s after the restart")
'

# argv: start end topic max_gap; stdin "topic payload". Fails if the topic had a gap longer than
# max_gap seconds between consecutive edge timestamps (including the window boundaries).
readonly CONTINUITY_CHECKER='
import json, sys
from datetime import datetime
def ts(text): return datetime.strptime(text, "%Y-%m-%dT%H:%M:%S.%fZ")
start, end, topic, max_gap = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
stamps = [start]
for line in sys.stdin:
    name, _, payload = line.rstrip("\n").partition(" ")
    if name == topic:
        stamp = json.loads(payload)["edge_timestamp"]
        if start <= stamp <= end:
            stamps.append(stamp)
stamps.append(end)
gap = max((ts(b) - ts(a)).total_seconds() for a, b in zip(stamps, stamps[1:]))
print(f"  {topic}: {len(stamps) - 2} messages, largest gap {gap:.1f}s (limit {max_gap:.0f}s)")
sys.exit(0 if gap <= max_gap else 1)
'

# Waits until every topic in the arguments has fresh telemetry in $1 (later than $2).
wait_for_fresh() {
    local file="$1" since="$2"
    shift 2
    local deadline=$((SECONDS + RECOVERY_TIMEOUT_S))
    until pyrun "$FRESHNESS_CHECKER" "$since" "$@" <"$file" >"$work/freshness.txt" 2>&1; do
        grep -q 'invalid payload' "$work/freshness.txt" && fail "$(cat "$work/freshness.txt")"
        ((SECONDS < deadline)) || fail "no fresh telemetry on $* within ${RECOVERY_TIMEOUT_S}s"
        sleep 1
    done
    cat "$work/freshness.txt"
}

require_log() { # $1 = logs, then extended regular expressions that must match
    local logs="$1" pattern
    shift
    for pattern in "$@"; do
        grep -Eq "$pattern" <<<"$logs" || fail "gateway log lacks '$pattern'"
    done
}

topics_seen() {
    local missing=0 topic
    for topic in "${OPCUA_TOPICS[@]}" "${MODBUS_TAGS[@]/#/$MODBUS_PREFIX/}"; do
        grep -q "^$topic " "$work/messages.txt" || missing=$((missing + 1))
    done
    return "$missing"
}

echo "==> building images"
compose build

echo "==> starting mosquitto, plant-simulator and modbus-simulator"
compose up -d --wait mosquitto plant-simulator modbus-simulator

echo "==> subscribing to industrial/# (before the gateway starts)"
start_subscriber "$work/messages.txt"

echo "==> starting edge-gateway with the mixed configuration"
compose up -d edge-gateway
gateway_id="$(compose ps -q edge-gateway)"
gateway_started_at="$(docker inspect -f '{{.State.StartedAt}}' "$gateway_id")"

echo "==> waiting for the 7 OPC UA and 11 Modbus topics"
deadline=$((SECONDS + TOPIC_TIMEOUT_S - 10))
until topics_seen; do
    ((SECONDS < deadline)) || fail "not all expected topics arrived within the time bound"
    kill -0 "$sub_pid" 2>/dev/null || fail "subscriber exited early: $(cat "$work/subscriber.err")"
    sleep 1
done
sleep 2 # a few more cycles of every source
stop_subscriber

echo "==> validating payloads, protocols, values and topics"
pyrun "$VALIDATOR" <"$work/messages.txt" || fail "payload validation failed"

echo "==> checking gateway logs"
require_log "$(compose logs --no-color edge-gateway)" \
    'event=started sources=2 ' \
    'event=source_started source_id=opcua-main protocol=opcua ' \
    'event=source_started source_id=compressor-plc protocol=modbus_tcp target=modbus-simulator:5020 items=11' \
    'component=opcua source_id=opcua-main .*state=Connected' \
    'component=modbus_tcp source_id=compressor-plc .*state=Connected' \
    'component=mqtt .*state=Connected'

echo "==> restarting only modbus-simulator: Modbus must recover, OPC UA must not be interrupted"
start_subscriber "$work/modbus-restart.txt"
window_start="$(utc_now)"
compose restart modbus-simulator >/dev/null
since="$(utc_now)"
wait_for_fresh "$work/modbus-restart.txt" "$since" "${MODBUS_CONTINUOUS[@]}"
sleep 2
window_end="$(utc_now)"
stop_subscriber
pyrun "$CONTINUITY_CHECKER" "$window_start" "$window_end" "$OPCUA_CONTINUOUS" "$MAX_OPCUA_GAP_S" \
    <"$work/modbus-restart.txt" || fail "OPC UA publishing was interrupted by the Modbus restart"
# The Modbus outage as subscribers see it (simulator stop + start + gateway reconnect).
pyrun "$CONTINUITY_CHECKER" "$window_start" "$window_end" "${MODBUS_CONTINUOUS[0]}" \
    "$RECOVERY_TIMEOUT_S" <"$work/modbus-restart.txt" || fail "Modbus outage longer than expected"
assert_same_gateway_running
logs="$(compose logs --no-color --since "$window_start" edge-gateway)"
require_log "$logs" \
    'component=modbus_tcp source_id=compressor-plc .*state=Disconnected' \
    'component=modbus_tcp source_id=compressor-plc .*state=Connected'
! grep -Eq 'component=opcua .*state=Disconnected' <<<"$logs" ||
    fail "the OPC UA source disconnected during the Modbus restart"
compose logs --no-color modbus-simulator | grep -q '"event": "stopped"' ||
    fail "modbus-simulator did not log a clean stop on SIGTERM"

echo "==> restarting mosquitto: OPC UA and Modbus telemetry must flow again"
before="$(utc_now)"
compose restart mosquitto >/dev/null
since="$(utc_now)"
start_subscriber "$work/mqtt-restart.txt"
wait_for_fresh "$work/mqtt-restart.txt" "$since" "$OPCUA_CONTINUOUS" "${MODBUS_CONTINUOUS[@]}"
stop_subscriber
assert_same_gateway_running
require_log "$(compose logs --no-color --since "$before" edge-gateway)" \
    '\[mqtt\] event=connection_lost' '\[mqtt\] event=connected'

echo "==> stopping edge-gateway with SIGTERM"
assert_same_gateway_running
container="$gateway_id"
start=$SECONDS
compose stop -t "$STOP_GRACE_S" edge-gateway >/dev/null
elapsed=$((SECONDS - start))
exit_code="$(docker inspect -f '{{.State.ExitCode}}' "$container")"
gateway_logs="$(compose logs --no-color edge-gateway)"
((exit_code == 0)) || fail "gateway exit code $exit_code (137 means it was killed after the grace period)"
((elapsed < STOP_GRACE_S)) || fail "gateway took ${elapsed}s to stop"
require_log "$gateway_logs" 'event=shutdown_requested signal=SIGTERM' 'event=stopped'
echo "    exit code 0 after ${elapsed}s"

echo "PASS: MVP 2 mixed Compose demo smoke test"
