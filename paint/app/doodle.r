/* Memory partition for System 7 / MultiFinder (System 6 without MultiFinder ignores it).
   Doodle with an H = 384 model peaks at about 700K (model 384K, two 52K pages, code). */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	1100 * 1024,      /* preferred */
	800 * 1024        /* minimum */
};
