#!/bin/sh
for n in 8 12 16 24 32; do b=$((n-1)); for m in off parallel; do /tmp/jobsbench/lobby.sh p${n}_$m $m $b $((b+40)); done; done
echo SWEEP_DONE
