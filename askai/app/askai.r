/* Ask AI resources: menus, the About box, the Settings dialog, SIZE (System 7 / MultiFinder). */
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
	{ "About Ask AI\0xC9", noIcon, noKey, noMark, plain;
	  "-", noIcon, noKey, noMark, plain }
};

resource 'MENU' (129) {
	129, textMenuProc, allEnabled, enabled, "File",
	{ "New Chat", noIcon, "N", noMark, plain;
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
	131, textMenuProc, 0x7FFFFFF9, enabled, "Connection",
	{ "Connect", noIcon, "K", noMark, plain;
	  "Stop Reply", noIcon, ".", noMark, plain;
	  "-", noIcon, noKey, noMark, plain;
	  "Modem Terminal", noIcon, noKey, noMark, plain;
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
	    "Ask AI 1.0\r\r"
	    "Chat with today's AI models (Claude, GPT, Gemini and more) from a 1986 Macintosh, "
	    "through a Wi-Fi modem on the modem port and the Ask AI relay, which keeps the API keys.\r\r"
	    "Made by Claude for Devon, 2026. Built with Retro68." } }
};

/* Settings. Positions are for a 430 x 286 dialog; the app centres it on the screen. */
resource 'DLOG' (200) {
	{0, 0, 286, 430}, dBoxProc, invisible, noGoAway, 0x0, 200, "", noAutoCenter
};

resource 'DITL' (200) {
	{ /*  1 */ {254, 350, 274, 420}, Button { enabled, "OK" };
	  /*  2 */ {254, 262, 274, 332}, Button { enabled, "Cancel" };
	  /*  3 */ {254, 10, 274, 90}, Button { enabled, "Defaults" };
	  /*  4 */ {10, 10, 26, 182}, StaticText { disabled, "Relay address (host:port):" };
	  /*  5 */ {10, 188, 26, 418}, EditText { enabled, "" };
	  /*  6 */ {36, 10, 52, 136}, StaticText { disabled, "Modem init string:" };
	  /*  7 */ {36, 142, 52, 300}, EditText { enabled, "" };
	  /*  8 */ {36, 316, 52, 348}, StaticText { disabled, "PIN:" };
	  /*  9 */ {36, 352, 52, 418}, EditText { enabled, "" };
	  /* 10 */ {62, 10, 78, 58}, StaticText { disabled, "Speed:" };
	  /* 11 */ {60, 60, 78, 118}, RadioButton { enabled, "1200" };
	  /* 12 */ {60, 120, 78, 178}, RadioButton { enabled, "2400" };
	  /* 13 */ {60, 180, 78, 238}, RadioButton { enabled, "9600" };
	  /* 14 */ {60, 240, 78, 304}, RadioButton { enabled, "19200" };
	  /* 15 */ {60, 306, 78, 370}, RadioButton { enabled, "38400" };
	  /* 16 */ {60, 372, 78, 430}, RadioButton { enabled, "57600" };
	  /* 17 */ {86, 10, 102, 88}, StaticText { disabled, "Handshake:" };
	  /* 18 */ {84, 92, 102, 152}, RadioButton { enabled, "None" };
	  /* 19 */ {84, 156, 102, 246}, RadioButton { enabled, "XON/XOFF" };
	  /* 20 */ {84, 250, 102, 418}, RadioButton { enabled, "Hardware (CTS)" };
	  /* 21 */ {110, 10, 126, 88}, StaticText { disabled, "Port:" };
	  /* 22 */ {108, 92, 126, 170}, RadioButton { enabled, "Modem" };
	  /* 23 */ {108, 174, 126, 260}, RadioButton { enabled, "Printer" };
	  /* 24 */ {132, 10, 150, 300}, CheckBox { enabled, "Connect when Ask AI starts" };
	  /* 25 */ {158, 10, 174, 420}, StaticText { disabled, "Instructions for the model (empty: the relay's own):" };
	  /* 26 */ {181, 13, 245, 417}, EditText { enabled, "" };
	  /* 27 */ {250, 346, 278, 424}, UserItem { disabled };
	}
};
