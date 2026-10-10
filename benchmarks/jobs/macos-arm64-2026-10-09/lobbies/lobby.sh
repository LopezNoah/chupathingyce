#!/bin/sh
# usage: lobby.sh <label> <HALO_JOBS> <bots> <exit_after_seconds>
label=$1; mode=$2; bots=$3; secs=$4
out=/tmp/jobsbench/lobby_$label; rm -rf $out; mkdir -p $out/saves
cp /tmp/chupathingy-play/saves/config.toml /tmp/chupathingy-play/saves/shader_cache.bin $out/saves/
cd /Users/noahlopez/Development/Github/chupathingyce
env HALO_DATA_ROOT="/Users/noahlopez/Library/Application Support/ChupathingyCE" HALO_SAVE_ROOT=$out/saves \
  HALO_FRAME_STATS=$out/frames.csv HALO_JOB_TRACE=$out/jobs.json HALO_JOBS=$mode \
  HALO_NETWORK_TEST=host:bloodgulch:slayer HALO_NETWORK_TEST_START=5 HALO_SOLO_GAME=1 HALO_BOTS=$bots HALO_BOT_SKILL=spartan \
  HALO_NET_ONLINE=false HALO_NET_ALLOW_UPNP=false HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false \
  HALO_NO_VSYNC=1 HALO_MAX_FPS=-1 HALO_EXIT_AFTER=$secs build/macos/halo > $out/game.log 2>&1
echo "$label exit $? bots_joined=$(grep -c 'joined' $out/game.log)"
