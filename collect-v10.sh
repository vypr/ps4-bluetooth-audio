#!/usr/bin/env bash
set -euo pipefail
PS4="${1:-192.168.1.104}"
LABEL="${2:-manual-recovery}"
case "$LABEL" in *[!a-zA-Z0-9_-]*|'') echo 'Label must contain only letters, numbers, dash or underscore' >&2; exit 2;; esac
OUT="$HOME/fw14-v10-results/$LABEL"
mkdir -p "$OUT"
curl -fsS --max-time 15 "ftp://$PS4:2121/data/bta_audio/status.json" -o "$OUT/status.json"
curl -fsS --max-time 15 "ftp://$PS4:2121/data/bta_audio/service.log" -o "$OUT/service.log"
echo '===== V10 HEADSET / RECOVERY STATUS ====='
python3 - "$OUT/status.json" <<'PY'
import json,sys
with open(sys.argv[1],encoding='utf-8') as f: s=json.load(f)
for k in ('api','selected','name','state','streaming','joint','send_type','volume',
          'latency_mode','tune_poll_us','tune_cushion_ms','tune_margin_ms',
          'v10_reset_count','v10_reset_kind','v10_reset_pending','v10_reset_wait_ms',
          'tune_trim_5s_ms','tune_late8_5s','tune_maxlate_5s_us',
          'packets','drops','errors','audio_rc'):
 print(f'{k}={s.get(k)}')
PY
echo
echo '===== BUILD, MANUAL RECOVERY, STREAM AND CHANNEL EVENTS ====='
grep -E 'FW14 V10|FW14 DIAG6 (stream start|stream stop|start send)|FW14 REC5: (observed|confirmed|per-link|link cycle)|radio:|link: dropped|connect: (link|dial)' "$OUT/service.log" | tail -n 70 || true
echo
echo '===== LAST BLUETOOTH LINK AND SEND ====='
grep -E 'FW14 (V7 LINK|DIAG6 SEND|V8 METRICS|V9 TUNE)' "$OUT/service.log" | tail -n 18 || true
printf '\nSaved files to %s\n' "$OUT"
