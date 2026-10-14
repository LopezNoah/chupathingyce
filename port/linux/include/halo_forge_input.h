/*
HALO_FORGE_INPUT.H

The keyboard and mouse for Forge, the map editor in the game
(port/linux/game/forge.c), from the platform layer
(port/linux/src/xinput_sdl.c). While Forge is open the keyboard and mouse
move its camera and edit the map: the player's actions get none of them,
nor does the player's view get the mouse.

Shared by the game (Xbox ABI) and the platform layer (the host's): plain
ints and floats only.
*/

#ifndef HALO_FORGE_INPUT_H
#define HALO_FORGE_INPUT_H

enum halo_forge_key
{
	/* forge.toggle_key in config.toml */
	HALO_FORGE_KEY_TOGGLE,
	HALO_FORGE_KEY_W,
	HALO_FORGE_KEY_A,
	HALO_FORGE_KEY_S,
	HALO_FORGE_KEY_D,
	HALO_FORGE_KEY_SPACE,
	HALO_FORGE_KEY_C,
	HALO_FORGE_KEY_SHIFT,
	HALO_FORGE_KEY_CTRL,
	HALO_FORGE_KEY_ALT,
	HALO_FORGE_KEY_E,
	HALO_FORGE_KEY_Q,
	HALO_FORGE_KEY_R,
	HALO_FORGE_KEY_P,
	HALO_FORGE_KEY_V,
	HALO_FORGE_KEY_G,
	HALO_FORGE_KEY_H,
	HALO_FORGE_KEY_Z,
	HALO_FORGE_KEY_Y,
	HALO_FORGE_KEY_DELETE,
	HALO_FORGE_KEY_ESCAPE,
	HALO_FORGE_KEY_LEFT_BRACKET,
	HALO_FORGE_KEY_RIGHT_BRACKET,
	HALO_FORGE_KEY_UP,
	HALO_FORGE_KEY_DOWN,
	HALO_FORGE_KEY_LEFT,
	HALO_FORGE_KEY_RIGHT,
	HALO_FORGE_KEY_PAGE_UP,
	HALO_FORGE_KEY_PAGE_DOWN,
	HALO_FORGE_KEY_MOUSE_LEFT,
	HALO_FORGE_KEY_MOUSE_RIGHT,
	HALO_FORGE_KEY_X,
	NUMBER_OF_HALO_FORGE_KEYS
};

struct halo_forge_input
{
	/* nonzero for each key held */
	unsigned char keys[NUMBER_OF_HALO_FORGE_KEYS];
	/* the mouse's motion (pixels) and wheel (notches, up positive) since the
	last read that captured them */
	float mouse_dx, mouse_dy;
	float wheel;
	/* zero while the console or a menu is up, or the window is not focused:
	keys then read as up */
	int available;
};

/* reads the keyboard and mouse; capture takes the mouse's motion and wheel
(Forge open), which the player's view then never gets */
void halo_forge_input_read(struct halo_forge_input *input, int capture);

/* whether an editor (Forge) is open (source/extensions/extension_dispatch.c):
the keyboard and mouse then drive no player action and turn no player's view
(xinput_sdl.c) */
int halo_extensions_input_captured(void);

#endif
