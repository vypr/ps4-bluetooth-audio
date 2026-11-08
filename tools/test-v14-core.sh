#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
cat > build/v14_core_main.c <<'C'
#include "test.h"
struct test_counts g_test = {0, 0};
void test_v8(void);
void test_v9_tune(void);
void test_v10_recovery(void);
void test_v11_volume(void);
void test_v12_tone(void);
void test_v13_sender(void);
void test_v14_hdmi(void);
int main(void) {
    test_json(); test_auth(); test_strfmt(); test_dsp();
    test_conn(); test_profiles(); test_timing(); test_diag_v6();
    test_pacing(); test_kernel(); test_v8(); test_v9_tune(); test_v10_recovery();
    test_v11_volume(); test_v12_tone(); test_v13_sender(); test_v14_hdmi();
    printf("V14 HDMI-UNLOCK: %d checks, %d failures\n",g_test.checks,g_test.failures);
    return g_test.failures ? 1 : 0;
}
C
cc -O2 -Wall -Wextra -Werror -Wno-unused-function \
    -DBTA_V7_SPACEBUDS_RECOVER=1 -DBTA_V8_FREEPODS_GAIN_PCT=130 \
    -DBTA_V8_PACE_CATCHUP=0 -I app -I tests \
    build/v14_core_main.c \
    tests/test_json.c tests/test_auth.c tests/test_strfmt.c tests/test_dsp.c \
    tests/test_conn.c tests/test_profiles.c tests/test_timing.c tests/test_diag_v6.c \
    tests/test_pacing.c tests/test_kernel.c tests/test_v8.c tests/test_v9_tune.c \
    tests/test_v10_recovery.c tests/test_v11_volume.c tests/test_v12_tone.c \
    tests/test_v13_sender.c tests/test_v14_hdmi.c \
    -o build/v14_core_tests
build/v14_core_tests
