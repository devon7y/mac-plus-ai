/* Memory partition for System 7 / MultiFinder (System 6 without MultiFinder ignores it). */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	600 * 1024,       /* preferred */
	400 * 1024        /* minimum */
};
