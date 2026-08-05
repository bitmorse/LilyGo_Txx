#!/usr/bin/env bash
# Host-side test suite for the pure-C logic (MCAP writer + protobuf encoder).
# Unit tests need only a C compiler; the optional integration test additionally
# validates a real MCAP file against the official `mcap` + protobuf libraries
# (skipped with a notice if they aren't installed in the venv).
#
#   make test        # or: tools/run_tests.sh
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -I main -I test"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

fail=0

echo "== unit tests =="
$CC $CFLAGS -o "$OUT/test_accel_encode" test/test_accel_encode.c main/accel_encode.c
"$OUT/test_accel_encode" || fail=1

$CC $CFLAGS -o "$OUT/test_mcap" test/test_mcap.c main/mcap.c
"$OUT/test_mcap" || fail=1

$CC $CFLAGS -o "$OUT/test_uartrx_ring" test/test_uartrx_ring.c main/uartrx_ring.c
"$OUT/test_uartrx_ring" || fail=1

$CC $CFLAGS -o "$OUT/test_uartrx_sm" test/test_uartrx_sm.c main/uartrx_sm.c
"$OUT/test_uartrx_sm" || fail=1

$CC $CFLAGS -o "$OUT/test_uartrx_rec" test/test_uartrx_rec.c main/uartrx_rec.c
"$OUT/test_uartrx_rec" || fail=1

$CC $CFLAGS -o "$OUT/test_wifi_creds" test/test_wifi_creds.c main/wifi_creds.c
"$OUT/test_wifi_creds" || fail=1

echo
echo "== integration: write a real MCAP with the production units =="
$CC $CFLAGS -o "$OUT/integration_write" \
    test/integration_write.c main/mcap.c main/accel_encode.c -lm
"$OUT/integration_write" "$OUT/viblog_test.mcap"

# Validate against the reference libraries if the venv has them.
PY=".venv/bin/python"
[ -x "$PY" ] || PY="python3"
if "$PY" -c "import mcap, google.protobuf" 2>/dev/null; then
    "$PY" test/validate_mcap.py "$OUT/viblog_test.mcap" || fail=1
else
    echo "  (skipped reference validation -- 'mcap' + 'protobuf' not installed;"
    echo "   uv pip install --python .venv mcap protobuf  to enable it)"
fi

# UART RX MCAP: schemaless /uart_rx raw + /state,/meta json.
$CC $CFLAGS -o "$OUT/integration_uart_write" \
    test/integration_uart_write.c main/mcap.c main/uartrx_rec.c
"$OUT/integration_uart_write" "$OUT/uart_test.mcap"
if "$PY" -c "import mcap" 2>/dev/null; then
    "$PY" test/validate_uart_mcap.py "$OUT/uart_test.mcap" || fail=1
else
    echo "  (skipped uart mcap validation -- 'mcap' not installed)"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "TESTS FAILED"
fi
exit "$fail"
