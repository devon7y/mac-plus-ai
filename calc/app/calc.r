/* Memory partition for System 7 / MultiFinder (System 6 without MultiFinder ignores it).
   Scribble Calc needs about 200K (strokes 32K, code about 60K, paper bitmap 9K). */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	384 * 1024,       /* preferred */
	256 * 1024        /* minimum */
};
