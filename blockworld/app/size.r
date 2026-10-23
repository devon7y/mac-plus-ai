/* Memory partition for System 7 / MultiFinder (System 6 without MultiFinder ignores it: the game
   runs as the shell or from the Finder and gets all the memory). Blockworld uses about 2.1 MB
   (the world 16,384 columns x 48 blocks, its segment lists, the view buffers, the code) plus up
   to about 0.2 MB while loading, saving or making a world (measured on the System 7.0.1 SE
   testbed, see HANDOFF.md M7). */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved, ignoreSuspendResumeEvents, reserved, cannotBackground, needsActivateOnFGSwitch,
	backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, not32BitCompatible,
	notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
	reserved, reserved, reserved,
	3072 * 1024,      /* preferred */
	2560 * 1024       /* minimum */
};
