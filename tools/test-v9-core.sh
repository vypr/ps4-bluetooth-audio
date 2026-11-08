#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
cat > build/v9_core_main.c <<'C'
#include "test.h"
struct test_counts g_test = {0, 0};
void test_v8(void);
void test_v9_tune(void);
int main(void) {
    test_json(); test_auth(); test_strfmt(); test_dsp();
    test_conn(); test_profiles(); test_timing(); test_diag_v6();
    test_pacing(); test_kernel(); test_v8(); test_v9_tune();
    printf("V9 live tuner: %d checks, %d failures\n",g_test.checks,g_test.failures);
    return g_test.failures ? 1 : 0;
}
C
cc -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -DBTA_V7_SPACEBUDS_RECOVER=1 -DBTA_V8_FREEPODS_GAIN_PCT=130 \
    -DBTA_V8_PACE_CATCHUP=0 -I app -I tests \
    build/v9_core_main.c \
    tests/test_json.c tests/test_auth.c tests/test_strfmt.c tests/test_dsp.c \
    tests/test_conn.c tests/test_profiles.c tests/test_timing.c tests/test_diag_v6.c \
    tests/test_pacing.c tests/test_kernel.c tests/test_v8.c tests/test_v9_tune.c \
    -o build/v9_core_tests
build/v9_core_tests
