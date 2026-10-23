/* Memory partition for System 7 / MultiFinder (System 6 without MultiFinder ignores it).
   Dream with the A64L2 model needs about 2.5 MB (1.6 MB of compiled layers, tables, tensors). */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	3000 * 1024,      /* preferred */
	2200 * 1024       /* minimum */
};
