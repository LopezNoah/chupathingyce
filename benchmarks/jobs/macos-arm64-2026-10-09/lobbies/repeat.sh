#!/bin/sh
for n in 24 32; do b=$((n-1)); for m in parallel off; do /tmp/jobsbench/lobby.sh r${n}_$m $m $b $((b+40)); done; done
echo REPEAT_DONE
