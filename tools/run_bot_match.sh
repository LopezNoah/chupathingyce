#!/bin/sh
# Visible, private repo-binary bot smoke test. Run from any directory.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
BINARY=${BOT_BINARY:-"$ROOT/build/macos/halo"}
DATA=${BOT_DATA_ROOT:-"$ROOT/build/macos/botrun/data"}
SAVES=${BOT_SAVE_ROOT:-"$ROOT/build/macos/botrun/saves"}
if [ ! -x "$BINARY" ]; then
	echo "Build first: python3 configure.py --bots && ninja macos" >&2
	exit 1
fi
if [ ! -f "$DATA/maps/bloodgulch.map" ] || [ ! -f "$DATA/maps/ui.map" ]; then
	echo "Set BOT_DATA_ROOT to an isolated data root containing your Xbox maps/ folder." >&2
	exit 1
fi
mkdir -p "$SAVES"
# Do not truncate debug.txt or erase existing run data; the game owns its log.
unset HALO_HIDDEN_WINDOW HALO_NULL_RENDERER HALO_NAV_PROBE HALO_BOT_SANDBOX
exec env HALO_DATA_ROOT="$DATA" HALO_SAVE_ROOT="$SAVES" \
	HALO_NET_ONLINE=false HALO_NET_PUBLIC_LOBBY=false \
	HALO_NET_HOST_PUBLIC=false HALO_NET_LIST_GAMES=false \
	HALO_NET_REPORT_GAMES=false HALO_NET_REPORT_EVENTS=false \
	HALO_NET_ALLOW_UPNP=false HALO_SOLO_GAME=1 \
	HALO_NETWORK_TEST="host:bloodgulch:team_slayer" HALO_NETWORK_TEST_START=5 \
	HALO_NETWORK_TEST_LOADOUT=rifle_pistol \
	HALO_BOTS="${BOT_COUNT:-24}" HALO_BOT_SKILL="${BOT_SKILL:-spartan}" \
	HALO_BOT_DECISIONS=1 HALO_EXIT_AFTER="${BOT_SECONDS:-180}" \
	"$BINARY"
