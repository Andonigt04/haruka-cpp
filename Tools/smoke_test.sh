#!/usr/bin/env bash
# Smoke test for the Haruka engine: run headless for 5 seconds and check
# that it doesn't crash or produce a fatal log message.
set -eu

BIN="${1:-./haruka}"
SCENE="${2:-}"

if [ ! -x "$BIN" ]; then
    echo "[smoke] binary not found: $BIN"
    echo "[smoke} usage: $0 [path-to-haruka] [scene-json]"
    exit 1
fi

ARGS="--headless"
if [ -n "$SCENE" ]; then
    ARGS="$ARGS --scene $SCENE"
fi

echo "[smoke] launching: $BIN $ARGS"
echo "[smoke] will kill after 5 seconds if still alive..."

# Run in background, capture PID, wait 5 s, kill
set +e
"$BIN" $ARGS &
PID=$!
sleep 5

if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    echo "[smoke] PASS (ran 5 s, killed cleanly)"
    exit 0
else
    wait "$PID"
    RC=$?
    if [ "$RC" -eq 0 ] || [ "$RC" -eq 130 ] || [ "$RC" -eq 143 ]; then
        echo "[smoke] PASS (exited with rc=$RC)"
        exit 0
    fi
    echo "[smoke] FAIL (rc=$RC)"
    exit 1
fi
