#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/work/rng_sweep}"
SIM="${SIM:-$ROOT_DIR/sim/build/sim_system}"
PAYLOAD="${PAYLOAD:-$ROOT_DIR/sim/payloads/sys/random_noc_test.bin}"
START_SEED="${START_SEED:-1}"
END_SEED="${END_SEED:-100}"
MAX_CYCLES="${MAX_CYCLES:-250000}"
JOBS="${JOBS:-4}"
TRACE_ENABLED="${TRACE_ENABLED:-}"
USE_NIX_DEVELOP="${USE_NIX_DEVELOP:-1}"
FAIL_RESULTS="${FAIL_RESULTS:-}"
BACKEND="${BACKEND:---soft}"

payload_base="$(basename "$PAYLOAD")"
if [ -z "$FAIL_RESULTS" ]; then
  case "$payload_base" in
    random_noc_test.bin)
      FAIL_RESULTS="0xdead0002"
      ;;
  esac
fi

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/seed_*.log "$OUT_DIR"/seed_*.ascii "$OUT_DIR"/seed_*.status "$OUT_DIR"/seed_*.fst "$OUT_DIR"/summary.txt "$OUT_DIR"/aggregate.txt
rm -rf "$OUT_DIR"/seed_*.run

export OUT_DIR SIM PAYLOAD MAX_CYCLES TRACE_ENABLED USE_NIX_DEVELOP FAIL_RESULTS BACKEND

seq "$START_SEED" "$END_SEED" | xargs -P "$JOBS" -I {} sh -c '
  set -e
  seed="$1"
  log="$OUT_DIR/seed_${seed}.log"
  ascii="$OUT_DIR/seed_${seed}.ascii"
  status="$OUT_DIR/seed_${seed}.status"
  wave="$OUT_DIR/seed_${seed}.fst"
  run_dir="$OUT_DIR/seed_${seed}.run"

  rm -rf "$run_dir"
  mkdir -p "$run_dir"

  TRACE_ARGS=""
  if [ -n "$TRACE_ENABLED" ]; then
    TRACE_ARGS="--trace"
  fi

  if (
    cd "$run_dir"
    if [ "$USE_NIX_DEVELOP" = "1" ]; then
      nix develop -c "$SIM" "$PAYLOAD" $BACKEND --max-cycles "$MAX_CYCLES" --rng-seed "$seed" $TRACE_ARGS
    else
      "$SIM" "$PAYLOAD" $BACKEND --max-cycles "$MAX_CYCLES" --rng-seed "$seed" $TRACE_ARGS
    fi
  ) > "$ascii" 2> "$log"; then
    if [ -f "$run_dir/soft_trace.fst" ]; then
      mv "$run_dir/soft_trace.fst" "$wave"
    elif [ -f "$run_dir/trace.fst" ]; then
      mv "$run_dir/trace.fst" "$wave"
    fi

    result=""
    cycles=""
    timer=""

    result_line="$(grep -E "^\\[(System|Soft)\\] Result:" "$log" | tail -n 1 || true)"
    cycles_line="$(grep -E "^\\[(System|Soft)\\] Cycles:" "$log" | tail -n 1 || true)"
    timer_line="$(grep -E "^\\[(System|Soft)\\] Timer:" "$log" | tail -n 1 || true)"

    if [ -n "$result_line" ]; then
      result="$(printf "%s\n" "$result_line" | sed -E "s/^\\[(System|Soft)\\] Result: .*\\((0x[0-9a-f]+)\\)$/\\2/")"
    fi
    if [ -n "$cycles_line" ]; then
      cycles="$(printf "%s\n" "$cycles_line" | sed -E "s/^\[(System|Soft)\] Cycles: ([0-9]+)$/\2/")"
    fi
    if [ -n "$timer_line" ]; then
      timer="$(printf "%s\n" "$timer_line" | sed -E "s/^\[(System|Soft)\] Timer: ([0-9]+) cycles$/\2/")"
    fi

    cosim_done_line="$(grep -E "^\\[CoSim\\] Matched through completion: result 0x[0-9a-f]+ in [0-9]+ cycles$" "$log" | tail -n 1 || true)"
    cosim_limit_line="$(grep -E "^\\[CoSim\\] Matched through cycle limit [0-9]+$" "$log" | tail -n 1 || true)"
    cosim_timer_line="$(grep -E "^\\[CoSim\\] Timer: [0-9]+ cycles$" "$log" | tail -n 1 || true)"

    if [ -n "$cosim_timer_line" ]; then
      timer="$(printf "%s\n" "$cosim_timer_line" | sed -E "s/^\[CoSim\] Timer: ([0-9]+) cycles$/\1/")"
    fi

    if [ -n "$cosim_done_line" ]; then
      result="$(printf "%s\n" "$cosim_done_line" | sed -E "s/^\\[CoSim\\] Matched through completion: result (0x[0-9a-f]+) in ([0-9]+) cycles$/\\1/")"
      cycles="$(printf "%s\n" "$cosim_done_line" | sed -E "s/^\\[CoSim\\] Matched through completion: result (0x[0-9a-f]+) in ([0-9]+) cycles$/\\2/")"
    elif [ -n "$cosim_limit_line" ]; then
      result="na"
      cycles="$(printf "%s\n" "$cosim_limit_line" | sed -E "s/^\[CoSim\] Matched through cycle limit ([0-9]+)$/\1/")"
    fi

    if [ -n "$result" ] && [ -n "$cycles" ] && [ -z "$timer" ]; then
      timer="na"
    fi

    if [ -n "$result" ] && [ -n "$cycles" ] && [ -n "$timer" ]; then
      case ",${FAIL_RESULTS}," in
        *,"${result}",*)
          printf "FAIL seed=%s reason=payload-fail result=%s cycles=%s timer=%s\n" "$seed" "$result" "$cycles" "$timer" | tee "$status"
          exit 1
          ;;
      esac
      printf "PASS seed=%s result=%s cycles=%s timer=%s\n" "$seed" "$result" "$cycles" "$timer" | tee "$status"
    else
      printf "FAIL seed=%s reason=missing-summary\n" "$seed" | tee "$status"
      exit 1
    fi
  else
    if [ -f "$run_dir/soft_trace.fst" ]; then
      mv "$run_dir/soft_trace.fst" "$wave"
    elif [ -f "$run_dir/trace.fst" ]; then
      mv "$run_dir/trace.fst" "$wave"
    fi
    printf "FAIL seed=%s reason=sim-error\n" "$seed" | tee "$status"
    exit 1
  fi

  rm -rf "$run_dir"
' _ {}

cat "$OUT_DIR"/seed_*.status | sort -V > "$OUT_DIR/summary.txt"

pass_count="$(grep -c '^PASS ' "$OUT_DIR/summary.txt" || true)"
fail_count="$(grep -c '^FAIL ' "$OUT_DIR/summary.txt" || true)"
results="$(awk '/^PASS /{for(i=1;i<=NF;i++) if($i ~ /^result=/){sub(/^result=/, "", $i); print $i}}' "$OUT_DIR/summary.txt" | sort -u | paste -sd, -)"
cycles="$(awk '/^PASS /{for(i=1;i<=NF;i++) if($i ~ /^cycles=/){sub(/^cycles=/, "", $i); print $i}}' "$OUT_DIR/summary.txt" | sort -u | paste -sd, -)"
timers="$(awk '/^PASS /{for(i=1;i<=NF;i++) if($i ~ /^timer=/){sub(/^timer=/, "", $i); print $i}}' "$OUT_DIR/summary.txt" | sort -u | paste -sd, -)"

{
  printf 'total=%s ok=%s fail=%s\n' "$((END_SEED - START_SEED + 1))" "$pass_count" "$fail_count"
  printf 'results=%s\n' "$results"
  printf 'cycles=%s\n' "$cycles"
  printf 'timers=%s\n' "$timers"
} | tee "$OUT_DIR/aggregate.txt"

if [ "$fail_count" != "0" ]; then
  exit 1
fi