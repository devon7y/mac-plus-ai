/* Memory partition for System 7 / MultiFinder (System 6 without MultiFinder ignores it).
   Guess My Doodle needs about 600K: the network's generated code (~280K), its file, tensors,
   the six drawings and the sounds. */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	1024 * 1024,      /* preferred */
	700 * 1024        /* minimum */
};
