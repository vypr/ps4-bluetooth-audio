#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
PACE="${1:-0}"
case "$PACE" in 0|1) ;; *) echo 'Usage: tools/test-v8-core.sh [0|1]' >&2; exit 2;; esac
mkdir -p build
cat > build/v8_core_main.c <<'C'
#include "test.h"
struct test_counts g_test = {0, 0};
void test_v8(void);
int main(void) {
    test_json(); test_auth(); test_strfmt(); test_dsp();
    test_conn(); test_profiles(); test_timing(); test_diag_v6();
    test_pacing(); test_kernel(); test_v8();
    printf("V8 core pacing=%d: %d checks, %d failures\n", BTA_V8_PACE_CATCHUP, g_test.checks, g_test.failures);
    return g_test.failures ? 1 : 0;
}
C
cc -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -DBTA_V7_SPACEBUDS_RECOVER=1 -DBTA_V8_FREEPODS_GAIN_PCT=130 \
    -DBTA_V8_PACE_CATCHUP="$PACE" -I app -I tests \
    build/v8_core_main.c \
    tests/test_json.c tests/test_auth.c tests/test_strfmt.c tests/test_dsp.c \
    tests/test_conn.c tests/test_profiles.c tests/test_timing.c tests/test_diag_v6.c \
    tests/test_pacing.c tests/test_kernel.c tests/test_v8.c \
    -o "build/v8_core_tests_$PACE"
"build/v8_core_tests_$PACE"
