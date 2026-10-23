/* SIZE -1 for System 7 / MultiFinder (System 6 without MultiFinder ignores it): Pixel needs about
   1.8 MB for its brain in compact (B5) memory mode plus code and buffers. The real lab SE had
   2,267K free, so ask for 2,200K and accept 2,000K. */
#include "Processes.r"
#include "Menus.r"
#include "Dialogs.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	2200 * 1024,      /* preferred */
	2000 * 1024       /* minimum */
};

resource 'MENU' (128) {
	128, textMenuProc, 0x7FFFFFFD, enabled, apple,
	{ "About Pixel\0xC9", noIcon, noKey, noMark, plain;
	  "-", noIcon, noKey, noMark, plain }
};

resource 'MENU' (129) {
	129, textMenuProc, allEnabled, enabled, "File",
	{ "New Chat", noIcon, "N", noMark, plain;
	  "Quit", noIcon, "Q", noMark, plain }
};

resource 'ALRT' (130) {
	{30, 64, 256, 384}, 130,
	{ OK, visible, silent; OK, visible, silent; OK, visible, silent; OK, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (130) {
	{ {194, 238, 214, 304}, Button { enabled, "OK" };
	  {12, 16, 186, 304}, StaticText { disabled, "Pixel 1.0\r\rA cheerful little Macintosh from 1986 who lives on your desk.\r\rA 6.7-million-parameter chatbot that runs entirely on this Mac.\r\rMade by Claude for Devon, 2026. Built with Retro68." } }
};
