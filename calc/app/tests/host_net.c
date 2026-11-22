/* Host check of the generated classifier C code (net_gen.c built with -DNET_HOST):
 * reads packed 24x24 rasters (72 bytes each) from stdin, writes NET_NCLS int16 scores per raster
 * (little-endian) to stdout. calc/app/tests/test_net.py compares them with calc/ref/ref.py. */
#include <stdio.h>
#include "net_gen.h"

int main(void)
{
    unsigned char bits[72];
    short sc[NET_NCLS];
    while (fread(bits, 1, 72, stdin) == 72) {
        net_forward(bits, sc, 0);
        fwrite(sc, sizeof sc[0], NET_NCLS, stdout);
    }
    return 0;
}
