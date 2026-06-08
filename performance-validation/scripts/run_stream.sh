#!/usr/bin/env bash
# Launch a concurrent ADIOS2 writer+reader for a streaming engine (SST/DataMan)
# and capture both RESULT lines. Runs from a scratch dir so SST's .sst rendezvous
# file and any BP files stay isolated.
#
# Args: ENGINE BYTES STEPS NPUB NSUB PORT TAG [extra writer args...]
set -u
ENGINE=$1; BYTES=$2; STEPS=$3; NPUB=$4; NSUB=$5; PORT=$6; TAG=$7
BIN=/home/user/dtlmod-verification/adios/adios_bench
export LD_LIBRARY_PATH=/usr/local/lib:${LD_LIBRARY_PATH:-}
SCRATCH=$(mktemp -d /tmp/stream.XXXXXX)
cd "$SCRATCH" || exit 1
NAME="stream_${TAG}"

# Reader in background first for SST (writer Open blocks on RendezvousReaderCount);
# for DataMan the writer binds the port, so start writer first. We start both and
# let the engine's handshake sort ordering out; a hard timeout guards against hangs.
timeout 120 mpirun --allow-run-as-root -np "$NPUB" "$BIN" --engine "$ENGINE" --role writer \
    --bytes "$BYTES" --steps "$STEPS" --warmup 1 --name "$NAME" --port "$PORT" --tag "$TAG" \
    > w.out 2> w.err &
WPID=$!
# Tiny stagger so the writer can create the .sst file / bind the port.
sleep 0.5
timeout 120 mpirun --allow-run-as-root -np "$NSUB" "$BIN" --engine "$ENGINE" --role reader \
    --bytes "$BYTES" --steps "$STEPS" --warmup 1 --name "$NAME" --port "$PORT" --tag "$TAG" \
    > r.out 2> r.err &
RPID=$!

wait $WPID; WRC=$?
wait $RPID; RRC=$?

grep -h RESULT w.out r.out
if [ $WRC -ne 0 ] || [ $RRC -ne 0 ]; then
    echo "STREAM_FAIL ENGINE=$ENGINE rc_w=$WRC rc_r=$RRC tag=$TAG" >&2
    echo "--- w.err ---" >&2; tail -5 w.err >&2
    echo "--- r.err ---" >&2; tail -5 r.err >&2
fi
cd / && rm -rf "$SCRATCH"
