#!/usr/bin/env bash
# Regression test for scripts/lib/output-contains.sh (the SIGPIPE false failure of
# `compose logs ... | grep -q ...` under pipefail in scripts/compose-mvp2-smoke-test.sh).
# No Docker needed; runs in well under a second.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
# shellcheck source=scripts/lib/output-contains.sh
source scripts/lib/output-contains.sh

readonly NEEDLE='"event": "stopped"'
readonly BULK_BYTES=$((4 * 1024 * 1024)) # far more than a pipe buffer (64 KiB on Linux)

failures=0
check() { # description, expected status, actual status
    if [[ "$2" == "$3" ]]; then
        echo "ok   - $1"
    else
        echo "FAIL - $1 (expected status $2, got $3)"
        failures=$((failures + 1))
    fi
}
check_fails() { # description, actual status: any non-zero status
    if [[ "$2" != 0 ]]; then
        echo "ok   - $1 (status $2)"
    else
        echo "FAIL - $1 (expected a non-zero status, got 0)"
        failures=$((failures + 1))
    fi
}

# The simulator's log: the searched line comes first, then a lot of other output.
needle_then_bulk() {
    echo "$NEEDLE"
    head -c "$BULK_BYTES" /dev/zero | tr '\0' 'x'
    echo
}
bulk_then_needle() {
    head -c "$BULK_BYTES" /dev/zero | tr '\0' 'x'
    echo
    echo "$NEEDLE"
}
failing_command() {
    echo "$NEEDLE"
    return 3
}

status=0
output_contains "$NEEDLE" needle_then_bulk || status=$?
check "found when the text precedes large output (the CI failure)" 0 "$status"

status=0
output_contains "$NEEDLE" bulk_then_needle || status=$?
check "found when the text follows large output" 0 "$status"

status=0
output_contains '"event": "never"' needle_then_bulk || status=$?
check "missing text is reported as not found" 1 "$status"

status=0
output_contains "$NEEDLE" failing_command 2>/dev/null || status=$?
check "a failing command is reported, not hidden" 2 "$status"

# The same with SIGPIPE ignored, as on GitHub Actions runners (Node ignores SIGPIPE and children
# inherit that): writes after grep's exit then fail with EPIPE instead of killing the producer.
status=0
(trap '' PIPE && output_contains "$NEEDLE" needle_then_bulk) 2>/dev/null || status=$?
check "found before large output with SIGPIPE ignored (CI runners)" 0 "$status"

status=0
(trap '' PIPE && output_contains '"event": "never"' needle_then_bulk) 2>/dev/null || status=$?
check "missing text not found with SIGPIPE ignored" 1 "$status"

# Documents the defect being guarded against: the old pattern fails although the text is present,
# with 141 (SIGPIPE) by default or 1 (EPIPE write error) where SIGPIPE is ignored.
status=0
needle_then_bulk 2>/dev/null | grep -q -- "$NEEDLE" || status=$?
check_fails "old pattern 'cmd | grep -q' fails under pipefail despite a match" "$status"

status=0
(trap '' PIPE && needle_then_bulk | grep -q -- "$NEEDLE") 2>/dev/null || status=$?
check_fails "old pattern also fails with SIGPIPE ignored" "$status"

((failures == 0)) || { echo "$failures check(s) failed"; exit 1; }
echo "PASS: output_contains"
