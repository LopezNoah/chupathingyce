# Forge implementation notes

Recorded after the initial local Forge implementation and runtime checks.

## Scope and design

- Forge is a local-only map editor, initially exercised on Blood Gulch. It is gated to 64-bit desktop builds; networking/synchronization is out of scope.
- Forge has its own active state rather than using `game_in_editor()`, whose broader engine effects are unsuitable for this mode.
- Edits are persisted in a separate `forge/<map>.forge.json` overlay under the data root. Original `.map` files are not modified. The overlay identifies the map/scenario and cache checksum so incompatible edits are not applied.
- It supports a flying camera; selection and manipulation of map-placed objects; placement, duplication and deletion; undo/redo; save/load; and an on-screen help HUD. Campaign Forge waits for the opening cutscene to finish. Keyboard/mouse is supported; gamepad editing is not implemented.
- Runtime objects created by scripts/game types and players are not generally editable. Multiplayer editing/synchronization, spawn points/game-type flags, encounter/trigger editing, pitch/roll rotation and a dedicated menu entry remain out of scope.

## Controls

Open/close with F7 or B (configurable as `forge.toggle_key`; macOS may require Fn+F7). Mouse look; WASD fly; Space/C vertical movement; click select/drop; right-click or E grab; Q/R rotate; arrows and PgUp/PgDn nudge; `[`/`]` or wheel choose an item; P place; V duplicate; Delete delete; G snapping; Ctrl+Z undo; Ctrl+Y redo; Ctrl+S save; H help; Esc put back/deselect.

The game is paused while Forge is open and the normal camera is restored on close. The host's loopback connection can show the game's “connection experiencing difficulties” warning while a solo System Link host is paused. This was observed in the test harness; it was not suppressed.

## Validation performed

- `ninja macos` completed successfully. A warning-enabled compile of `forge.c` was also checked.
- A scripted live-game edit test exercised opening, pausing, flying, object selection/movement/rotation, placing a Warthog, deleting, saving, undo/redo and closing.
- A separate fresh process loaded the overlay and verified all three edits on Blood Gulch and on campaign level The Silent Cartographer (`b30`). The vehicle had settled under physics, so verification allowed a small position tolerance.
- Screenshots showed the Forge HUD/selection outline and the normal first-person view restored after closing.
- This was not a manual keyboard/mouse test and did not include 32-bit Linux or Windows builds. 32-bit neutrality was reasoned from the feature guards and changed source locations, not established by running the port-neutrality checker.
- Test scratch data and screenshots were kept under `/tmp/forge-test`; the installed maps directory was symlinked for reading.

## `fix.patch` and the libtiff leak

`fix.patch` changes `source/bitmaps/libtiff/tif_getimage.c`'s `gtStripSeparate()` logic. The current code allocates `buf` and then calls `pickTileSeparateCase(Map)`. If that returns null, it returns without freeing `buf`, which is the leak noted in the source comment.

The patch calls `pickTileSeparateCase(Map)` and returns on null **before allocating** the buffer. This removes that leak path: there is no allocation to free when the unsupported format is rejected. For the specific leak, the ordering change is sufficient.

There is one regression in the patch as written: it also removes `TIFFError(filename, "Can not handle format")` from the unsupported-format path. Keep that diagnostic while moving the check before the allocation, for example:

```c
put = pickTileSeparateCase(Map);
if (put == 0) {
    TIFFError(filename, "Can not handle format");
    return (0);
}
stripsize = TIFFStripSize(tif);
r = buf = (u_char *)debug_malloc(3*stripsize, 0,
    TIF_GETIMAGE_FILE, 487);
```

Alternatively, retain the allocation order and explicitly `debug_free(buf, TIF_GETIMAGE_FILE, <matching allocation-site line>)` before returning. The move-before-allocation approach is simpler and avoids allocating needlessly. Note that the patch's file headers are relative to the libtiff directory (`a/tif_getimage.c`), so it may need to be applied from `source/bitmaps/libtiff` or with an adjusted strip level from the repository root.

**Conclusion:** `fix.patch` prevents the identified leak, but should be amended to preserve the `TIFFError` diagnostic before it is applied. The patch has not been applied or tested here.

## Files involved

Forge implementation: `port/linux/game/forge.c`, `port/linux/game/forge.h`, `port/linux/include/halo_forge_input.h`, `port/linux/src/xinput_sdl.c`, `port/linux/src/port_config.c`, `port/linux/game/render_interpolation.c`, plus hooks in `source/editor/editor_stubs.c`, `source/camera/director.c`, `source/game/game.c`, `source/main/main.c`, `source/objects/object_types.c`, and `source/objects/objects.c`.
