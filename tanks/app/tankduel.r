/* Tank Duel resources: SIZE -1 for System 7 / MultiFinder (600K preferred, 400K minimum). */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	600 * 1024,
	400 * 1024
};
