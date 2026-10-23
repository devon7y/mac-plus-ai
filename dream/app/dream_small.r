/* Memory partition for System 7 / MultiFinder (System 6 without MultiFinder ignores it).
   Dream for the 1 MB Plus (32x32 model, lookup-table kernels) needs about 600K. */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	700 * 1024,      /* preferred */
	560 * 1024       /* minimum */
};
