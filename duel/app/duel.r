/* Memory partition for System 7 / MultiFinder (System 6 without MultiFinder ignores it).
   Orbit Duel needs about 200K: three 22K screen buffers, code, the AppleTalk buffers. */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	384 * 1024,       /* preferred */
	256 * 1024        /* minimum */
};
