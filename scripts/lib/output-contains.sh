# shellcheck shell=bash
# Sourced by the smoke tests (no shebang, not executable).

# output_contains TEXT COMMAND [ARGS...]
#
# Succeeds if the output of COMMAND contains the fixed string TEXT; returns 1 if it does not and
# 2 if COMMAND itself fails (reported on stderr, never hidden).
#
# The whole output is captured before searching. `COMMAND | grep -q TEXT` is unsafe under
# `set -o pipefail`: grep -q exits at the first match, the producer then gets SIGPIPE (exit 141)
# on its next write, and pipefail turns that into a failed check although TEXT was found. That
# happens whenever more output follows the match than fits in the pipe buffer.
output_contains() {
    local text="$1" output
    shift
    if ! output="$("$@")"; then
        echo "output_contains: command failed: $*" >&2
        return 2
    fi
    grep -qF -- "$text" <<<"$output"
}
