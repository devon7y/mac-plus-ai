#include "Processes.r"
data 'TMDL' (128, purgeable) { $$read("model.bin") };
resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved, 900 * 1024, 700 * 1024
};
