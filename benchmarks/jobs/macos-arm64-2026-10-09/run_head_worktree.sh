#!/bin/sh
# baseline-equivalent run: usage base.sh <label> <HALO_JOBS>
label=$1; mode=$2
out=/tmp/jobsbench/head_$label; rm -rf $out; mkdir -p $out/saves
cp /tmp/chupathingy-play/saves/config.toml $out/saves/ 2>/dev/null
cp /tmp/chupathingy-play/saves/shader_cache.bin $out/saves/ 2>/dev/null
cd /tmp/chupa_head
env HALO_DATA_ROOT="/Users/noahlopez/Library/Application Support/ChupathingyCE" HALO_SAVE_ROOT=$out/saves \
  HALO_TRACE_FILE=$out/trace.json HALO_JOB_TRACE=$out/jobs.json HALO_JOBS=$mode \
  HALO_NETWORK_TEST=host:bloodgulch:slayer HALO_NETWORK_TEST_START=5 HALO_SOLO_GAME=1 HALO_BOTS=3 HALO_BOT_SKILL=spartan \
  HALO_NET_ONLINE=false HALO_NET_ALLOW_UPNP=false HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false \
  HALO_EXIT_AFTER=45 build/macos/halo > $out/game.log 2>&1 &
pid=$!
echo "seconds,rss_kib" > $out/rss.csv; t=0
while kill -0 $pid 2>/dev/null; do sleep 5; t=$((t+5)); r=$(ps -o rss= -p $pid 2>/dev/null | tr -d ' '); [ -n "$r" ] && echo "$t,$r" >> $out/rss.csv; done
wait $pid; echo "$label exit $?"
