#!/usr/bin/env bash
# Full ADIOS2 measurement sweep -> results/adios_raw.csv
# RESULT columns:
#   marker,tag,engine,role,nranks,bytes_per_rank,steps,open_s,step_s,close_s,
#   total_bytes,bw_GBps,min_step_s,med_step_s
#
# Engine rank limits dictated by ADIOS2 itself:
#   BP5     : any rank count (per-rank subfiles)
#   SST     : 1,2 here (writer+reader run concurrently; >4 total procs would
#             oversubscribe the 4-core box and distort timing)
#   DataMan : 1 only (DataManWriter throws for MpiSize>1)
set -u
ROOT=/home/user/dtlmod-verification
BIN=$ROOT/adios/adios_bench
OUT=$ROOT/results/adios_raw.csv
export LD_LIBRARY_PATH=/usr/local/lib:${LD_LIBRARY_PATH:-}
MPIRUN="mpirun --allow-run-as-root --oversubscribe --bind-to none"

# Guard: refuse to run if a benchmark is already executing (would oversubscribe
# the CPU and corrupt timings). Match the actual invocation, not the script name.
others=$(pgrep -fc "adios_bench --engine" || true)
if [ "${others:-0}" -gt 0 ]; then
    echo "ABORT: $others adios_bench processes already running" >&2
    ps aux | grep "adios_bench --engine" | grep -v grep >&2
    exit 2
fi

echo "marker,tag,engine,role,nranks,bytes_per_rank,steps,open_s,step_s,close_s,total_bytes,bw_GBps,min_step_s,med_step_s" > "$OUT"

REPS=${REPS:-3}
SIZES="1024 16384 262144 1048576 4194304 16777216 67108864 134217728"
PORT=12400

steps_for() {  # echo "STEPS WARMUP" sized to the data so large runs don't blow up disk
    local s=$1
    if   [ "$s" -le 262144 ];   then echo "30 5"
    elif [ "$s" -le 16777216 ]; then echo "12 2"
    else                              echo "6 1"; fi
}

run_bp() {  # size ranks rep
    local size=$1 ranks=$2 rep=$3
    read STEPS WARM <<< "$(steps_for "$size")"
    local dir; dir=$(mktemp -d /tmp/bp.XXXXXX)
    timeout 300 $MPIRUN -np "$ranks" "$BIN" --engine BP5 --role writer \
        --bytes "$size" --steps "$STEPS" --warmup "$WARM" --name "$dir/out.bp" --tag "rep$rep" \
        2>>"$dir/err" | grep RESULT >> "$OUT"
    timeout 300 $MPIRUN -np "$ranks" "$BIN" --engine BP5 --role reader \
        --bytes "$size" --steps "$STEPS" --warmup "$WARM" --name "$dir/out.bp" --tag "rep$rep" \
        2>>"$dir/err" | grep RESULT >> "$OUT"
    rm -rf "$dir"
}

run_stream() {  # engine size ranks rep port
    local engine=$1 size=$2 ranks=$3 rep=$4 port=$5
    read STEPS WARM <<< "$(steps_for "$size")"
    local dir; dir=$(mktemp -d /tmp/st.XXXXXX); cd "$dir"
    local name="s_${engine}_${size}_${ranks}_${rep}"
    timeout 240 $MPIRUN -np "$ranks" "$BIN" --engine "$engine" --role writer \
        --bytes "$size" --steps "$STEPS" --warmup "$WARM" --name "$name" --port "$port" --tag "rep$rep" \
        > w.out 2> w.err &
    local wpid=$!
    sleep 0.5
    timeout 240 $MPIRUN -np "$ranks" "$BIN" --engine "$engine" --role reader \
        --bytes "$size" --steps "$STEPS" --warmup "$WARM" --name "$name" --port "$port" --tag "rep$rep" \
        > r.out 2> r.err &
    local rpid=$!
    wait $wpid; local wrc=$?
    wait $rpid; local rrc=$?
    grep -h RESULT w.out r.out >> "$OUT"
    [ $wrc -ne 0 -o $rrc -ne 0 ] && { echo "FAIL $engine sz=$size r=$ranks rep=$rep w=$wrc r=$rrc" >&2; tail -3 w.err r.err >&2; }
    cd /; rm -rf "$dir"
}

echo "### BP5 (ranks 1,2,4) ###" >&2
for size in $SIZES; do for ranks in 1 2 4; do for rep in $(seq 0 $((REPS-1))); do
    run_bp "$size" "$ranks" "$rep"
done; done; done

echo "### SST (ranks 1,2) ###" >&2
for size in $SIZES; do for ranks in 1 2; do for rep in $(seq 0 $((REPS-1))); do
    PORT=$((PORT+2)); run_stream SST "$size" "$ranks" "$rep" "$PORT"
done; done; done

echo "### DataMan (rank 1) ###" >&2
for size in $SIZES; do for ranks in 1; do for rep in $(seq 0 $((REPS-1))); do
    PORT=$((PORT+2)); run_stream DataMan "$size" "$ranks" "$rep" "$PORT"
done; done; done

echo "### DONE: $(grep -c RESULT "$OUT") rows ###" >&2
