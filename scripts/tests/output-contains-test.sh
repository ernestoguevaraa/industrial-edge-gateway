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

# Documents the defect being guarded against: the old pattern fails (SIGPIPE, 141) although the
# text is present.
status=0
needle_then_bulk | grep -q -- "$NEEDLE" || status=$?
check "old pattern 'cmd | grep -q' fails under pipefail despite a match" 141 "$status"

((failures == 0)) || { echo "$failures check(s) failed"; exit 1; }
echo "PASS: output_contains"
