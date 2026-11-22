/* Tune's resources: the model, the memory partition, version. */
#include "Processes.r"
#include "MacTypes.r"

data 'TMDL' (128, purgeable) { $$read("model.bin") };

/* System 7 / MultiFinder partition: model ~380K, bass table 64K, staff and work ~80K, code ~60K */
resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	800 * 1024,
	700 * 1024
};

resource 'vers' (1) {
	0x01, 0x00, release, 0x00, 0,
	"1.0",
	"1.0, the Mac composes folk tunes (2026)"
};
