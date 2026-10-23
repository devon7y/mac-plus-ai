/* SIZE -1: ask for 16 MB (preferred), accept 384K (minimum). Under System 7 / MultiFinder
   the Process Manager then gives this app the largest unused block, which it reports. */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	16384 * 1024,     /* preferred */
	384 * 1024        /* minimum */
};
