#!/usr/bin/env bash
# V6's supplied source zip omitted native/model.h and native/qr.h, so the
# original `make test` cannot run as extracted. This runs all the remaining
# core suites, including the V7 kernel whitelist and recovery-gate tests.
set -euo pipefail
cd "$(dirname "$0")/.."
MODE="${1:-0}"
case "$MODE" in 0|1) ;; *) echo 'Usage: tools/test-v7-core.sh [0|1]' >&2; exit 2;; esac
mkdir -p build
cat > build/v7_core_main.c <<'C'
#include "test.h"
struct test_counts g_test = {0, 0};
int main(void) {
    test_json(); test_auth(); test_strfmt(); test_dsp();
    test_conn(); test_profiles(); test_timing(); test_diag_v6();
    test_pacing(); test_kernel();
    printf("V7 core: %d checks, %d failures\n", g_test.checks, g_test.failures);
    return g_test.failures ? 1 : 0;
}
C
cc -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -DBTA_V7_SPACEBUDS_RECOVER="$MODE" -I app -I tests \
    build/v7_core_main.c \
    tests/test_json.c tests/test_auth.c tests/test_strfmt.c tests/test_dsp.c \
    tests/test_conn.c tests/test_profiles.c tests/test_timing.c tests/test_diag_v6.c \
    tests/test_pacing.c tests/test_kernel.c -o "build/v7_core_tests_$MODE"
"build/v7_core_tests_$MODE"
