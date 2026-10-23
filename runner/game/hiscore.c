/* Best score: a 4-byte file next to the application (the default directory at launch). */
#include <Files.h>
#include "hiscore.h"

static const unsigned char fname[] = "\pRail Runner Best";

long hiscore_load(void)
{
    short ref;
    long v = 0, n = 4;
    if (FSOpen(fname, 0, &ref) != noErr)
        return 0;
    if (FSRead(ref, &n, &v) != noErr || n != 4)
        v = 0;
    FSClose(ref);
    return v < 0 ? 0 : v;
}

void hiscore_save(long score)
{
    short ref;
    long n = 4;
    OSErr e = FSOpen(fname, 0, &ref);
    if (e == fnfErr) {
        if (Create(fname, 0, 'RRun', 'BEST') != noErr)
            return;
        e = FSOpen(fname, 0, &ref);
    }
    if (e != noErr)
        return;
    SetFPos(ref, fsFromStart, 0);
    FSWrite(ref, &n, &score);
    FSClose(ref);
    FlushVol(0, 0);
}
