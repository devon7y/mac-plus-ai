/* Shuttle resources: menus, the About box, the error alert, the Settings dialog, SIZE. */
#include "Processes.r"
#include "Menus.r"
#include "Dialogs.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, canBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	384 * 1024,       /* preferred */
	256 * 1024        /* minimum */
};

resource 'MENU' (128) {
	128, textMenuProc, 0x7FFFFFFD, enabled, apple,
	{ "About Shuttle\0xC9", noIcon, noKey, noMark, plain;
	  "-", noIcon, noKey, noMark, plain }
};

resource 'MENU' (129) {
	129, textMenuProc, 0x7FFFFFF7, enabled, "File",
	{ "Download", noIcon, "D", noMark, plain;
	  "Send File\0xC9", noIcon, "U", noMark, plain;
	  "Cancel Transfer", noIcon, ".", noMark, plain;
	  "-", noIcon, noKey, noMark, plain;
	  "Quit", noIcon, "Q", noMark, plain }
};

resource 'MENU' (130) {
	130, textMenuProc, 0x7FFFFFFC, enabled, "Edit",
	{ "Undo", noIcon, "Z", noMark, plain;
	  "-", noIcon, noKey, noMark, plain;
	  "Cut", noIcon, "X", noMark, plain;
	  "Copy", noIcon, "C", noMark, plain;
	  "Paste", noIcon, "V", noMark, plain;
	  "Clear", noIcon, noKey, noMark, plain }
};

resource 'MENU' (131) {
	131, textMenuProc, 0x7FFFFFFB, enabled, "Connection",
	{ "Connect", noIcon, "K", noMark, plain;
	  "Refresh", noIcon, "R", noMark, plain;
	  "-", noIcon, noKey, noMark, plain;
	  "Modem Terminal", noIcon, "T", noMark, plain;
	  "Settings\0xC9", noIcon, noKey, noMark, plain }
};

resource 'ALRT' (130) {
	{40, 54, 262, 394}, 130,
	{ OK, visible, silent; OK, visible, silent; OK, visible, silent; OK, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (130) {
	{ {190, 258, 210, 324}, Button { enabled, "OK" };
	  {12, 16, 182, 324}, StaticText { disabled,
	    "Shuttle 1.0\r\r"
	    "Moves files between this Macintosh and a MacBook, through a Wi-Fi modem on the modem port. "
	    "The MacBook runs the Shuttle server; files keep their resource forks, types and creators.\r\r"
	    "Made by Claude for Devon, 2026. Built with Retro68." } }
};

resource 'ALRT' (131) {
	{40, 40, 196, 440}, 131,
	{ OK, visible, sound1; OK, visible, sound1; OK, visible, sound1; OK, visible, sound1 },
	alertPositionMainScreen
};

resource 'DITL' (131) {
	{ {124, 318, 144, 388}, Button { enabled, "OK" };
	  {10, 72, 118, 390}, StaticText { disabled, "^0" } }
};

/* Settings. Positions are for a 430 x 232 dialog; the app centres it on the screen. */
resource 'DLOG' (200) {
	{0, 0, 232, 430}, dBoxProc, invisible, noGoAway, 0x0, 200, "", noAutoCenter
};

resource 'DITL' (200) {
	{ /*  1 */ {200, 350, 220, 420}, Button { enabled, "OK" };
	  /*  2 */ {200, 262, 220, 332}, Button { enabled, "Cancel" };
	  /*  3 */ {200, 10, 220, 90}, Button { enabled, "Defaults" };
	  /*  4 */ {10, 10, 26, 190}, StaticText { disabled, "Dial (name or host:port):" };
	  /*  5 */ {10, 196, 26, 418}, EditText { enabled, "" };
	  /*  6 */ {36, 10, 52, 140}, StaticText { disabled, "Modem init string:" };
	  /*  7 */ {36, 146, 52, 300}, EditText { enabled, "" };
	  /*  8 */ {62, 10, 78, 58}, StaticText { disabled, "Speed:" };
	  /*  9 */ {60, 60, 78, 118}, RadioButton { enabled, "1200" };
	  /* 10 */ {60, 120, 78, 178}, RadioButton { enabled, "2400" };
	  /* 11 */ {60, 180, 78, 238}, RadioButton { enabled, "9600" };
	  /* 12 */ {60, 240, 78, 304}, RadioButton { enabled, "19200" };
	  /* 13 */ {60, 306, 78, 370}, RadioButton { enabled, "38400" };
	  /* 14 */ {60, 372, 78, 430}, RadioButton { enabled, "57600" };
	  /* 15 */ {86, 10, 102, 88}, StaticText { disabled, "Handshake:" };
	  /* 16 */ {84, 92, 102, 152}, RadioButton { enabled, "None" };
	  /* 17 */ {84, 156, 102, 246}, RadioButton { enabled, "XON/XOFF" };
	  /* 18 */ {84, 250, 102, 418}, RadioButton { enabled, "Hardware (CTS)" };
	  /* 19 */ {110, 10, 126, 88}, StaticText { disabled, "Port:" };
	  /* 20 */ {108, 92, 126, 170}, RadioButton { enabled, "Modem" };
	  /* 21 */ {108, 174, 126, 260}, RadioButton { enabled, "Printer" };
	  /* 22 */ {132, 10, 150, 300}, CheckBox { enabled, "Connect when Shuttle starts" };
	  /* 23 */ {156, 10, 190, 420}, StaticText { disabled,
	            "\"mac\" works once the modem knows it: type the AT+ALIAS line the Shuttle server prints, "
	            "in Connection > Modem Terminal." };
	  /* 24 */ {196, 346, 224, 424}, UserItem { disabled };
	}
};
