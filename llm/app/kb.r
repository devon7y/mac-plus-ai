/* PixelSE only: the Edit menu with the on-screen keyboard (main.c, PX_KB). */
#include "Menus.r"

resource 'MENU' (130) {
	130, textMenuProc, allEnabled, enabled, "Edit",
	{ "Keyboard\0xC9", noIcon, "K", noMark, plain }
};
