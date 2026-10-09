#!/bin/sh
# Open the full app directly into single-player offline Blood Gulch.
# Uses the local launcher, but no automated gameplay fixture.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app="$repo/build/macos/ChupathingyCE.app"
if [ ! -d "$app" ]; then
	echo "Build first: python3 configure.py --platform --infection; ninja macos" >&2
	exit 1
fi
open -n "$app" \
	--env HALO_PLATFORM_ENABLED=true \
	--env "HALO_PLATFORM_ASSET=$repo/blender/forge_grid.glb" \
	--env HALO_PLATFORM_TEST=false \
	--env "HALO_FORGE_SELECTOR_ASSET=$repo/blender/forge_gui.png" \
	--env "HALO_FORGE_SELECTED_SELECTOR_ASSET=$repo/blender/forge_selected_gui.png" \
	--env HALO_FORGE_SELECTOR_SIZE=32 \
	--env 'HALO_FORGE_TEST=' \
	--env HALO_INFECTION_LOCAL=false \
	--env HALO_INFECTION_TEST_MAP=bloodgulch \
	--env HALO_INFECTION_TEST_SCENARIO=local-play \
	--env HALO_INFECTION_TEST_PLAYERS=1 \
	--env 'HALO_NETWORK_TEST=' \
	--env HALO_NET_ONLINE=false \
	--env HALO_NET_ALLOW_UPNP=false \
	--env HALO_NET_REPORT_GAMES=false \
	--env HALO_NET_REPORT_EVENTS=false \
	--env HALO_NET_JOIN_FROM_CLIPBOARD=false \
	--env HALO_EXIT_AFTER=0
