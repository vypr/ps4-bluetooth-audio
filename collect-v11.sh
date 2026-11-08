#!/usr/bin/env bash
set -euo pipefail
PS4="${1:-192.168.1.104}"
LABEL="${2:-volume-boost}"
case "$LABEL" in *[!a-zA-Z0-9_-]*|'') echo 'Label must contain only letters, numbers, dash or underscore' >&2; exit 2;; esac
OUT="$HOME/fw14-v11-results/$LABEL"
mkdir -p "$OUT"
curl -fsS --max-time 15 "ftp://$PS4:2121/data/bta_audio/status.json" -o "$OUT/status.json"
curl -fsS --max-time 15 "ftp://$PS4:2121/data/bta_audio/service.log" -o "$OUT/service.log"
echo '===== V11 VOLUME BOOST / STREAM ====='
python3 - "$OUT/status.json" <<'PY'
import json,sys
with open(sys.argv[1],encoding='utf-8') as f: s=json.load(f)
for k in ('api','selected','name','state','streaming','joint','send_type','volume',
          'volume_max','volume_post_gain_pct','latency_mode','tune_poll_us',
          'tune_cushion_ms','tune_margin_ms','v10_reset_count','packets','errors','audio_rc'):
 print(f'{k}={s.get(k)}')
PY
echo
echo '===== V11 BUILD AND VOLUME CHANGES ====='
grep -E 'FW14 V11|FW14 V10 build=|FW14 V8 FreePods gain' "$OUT/service.log" | tail -n 25 || true
echo
echo '===== RECENT SEND / LIMITER METRICS ====='
grep -E 'FW14 (V8 METRICS|DIAG6 SEND|V7 LINK)' "$OUT/service.log" | tail -n 28 || true
printf '\nSaved files to %s\n' "$OUT"
