#!/bin/sh
# usage: run.sh <label> <HALO_JOBS> <workers>
label=$1; mode=$2; workers=$3
out=/tmp/jobsbench/$label; rm -rf $out; mkdir -p $out/saves
cd /Users/noahlopez/Development/Github/chupathingyce
env HALO_DATA_ROOT="/Users/noahlopez/Library/Application Support/ChupathingyCE" HALO_SAVE_ROOT=$out/saves HALO_TRACE_FILE=$out/trace.json HALO_JOB_TRACE=$out/jobs.json \
  HALO_JOBS=$mode HALO_JOB_WORKERS=$workers \
  HALO_NETWORK_TEST=host:bloodgulch:slayer HALO_NETWORK_TEST_START=5 HALO_SOLO_GAME=1 HALO_BOTS=3 HALO_BOT_SKILL=spartan \
  HALO_NET_ONLINE=false HALO_NET_ALLOW_UPNP=false HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false \
  HALO_NO_VSYNC=1 HALO_EXIT_AFTER=50 \
  build/macos/halo > $out/game.log 2>&1
echo "$label exit $?"
