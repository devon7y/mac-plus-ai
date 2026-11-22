/* Guess My Doodle: the drawing -> the classifier's 32x32 input (guess/engine/raster.py). */
#ifndef GUESS_RASTER_H
#define GUESS_RASTER_H

#define RMAXPTS 2000

/* xs, ys: canvas coordinates 0..255; up[k]: the pen is lifted after point k. bits: 128 bytes,
 * 32 rows of 4 bytes, MSB = left. */
void rasterize(const unsigned char *xs, const unsigned char *ys, const unsigned char *up, short n,
               unsigned char *bits);

#endif
