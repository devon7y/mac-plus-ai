/* Shuttle: files on the Mac's disks. Downloads arrive as a MacBinary stream that is written into
   both forks; uploads are read as a MacBinary II stream. */
#ifndef SHUTTLE_FILES_H
#define SHUTTLE_FILES_H
#include <MacTypes.h>

OSErr dl_begin(const unsigned char *name, short vRefNum, OSType type, OSType creator, long dlen, long rlen);
OSErr dl_write(const unsigned char *p, long n);   /* the next bytes of the MacBinary stream */
OSErr dl_finish(void);               /* closes the file and sets its type, creator, flags and dates */
OSErr dl_verify(unsigned char *buf, long n);  /* reads the saved file back and checks it (after dl_finish) */
void dl_abort(void);                 /* closes and deletes the partial file */
void dl_discard(void);               /* deletes the saved file (after dl_finish) */

OSErr ul_begin(const unsigned char *name, short vRefNum, long *total);
OSErr ul_read(unsigned char *p, long n);          /* the next bytes of the MacBinary II stream */
void ul_end(void);

long vol_free(short vRefNum, long *alBlk);        /* free bytes on a volume (-1 if unknown) */
long file_size(const unsigned char *name, short vRefNum);   /* both forks, -1 if there's no such file */
short startup_vol(void);             /* the startup volume's real reference number */
const char *files_errText(OSErr e);

#define ERR_MACBINARY (-30001)       /* the stream wasn't MacBinary / didn't match the listing */
#define ERR_VERIFY (-30002)          /* the saved file read back differently */

#endif
