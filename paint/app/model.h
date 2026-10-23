/* The drawing model (model.c). */
#ifndef MODEL_H
#define MODEL_H
OSErr ModelLoad(void);                       /* reads "Doodle Model" from the app's folder */
Boolean ModelLoaded(void);
short ModelNCat(void);
const unsigned char *ModelName(short i);     /* Pascal string */
void ModelBegin(short cat, unsigned long seed, short temp);   /* fold, h0, tables */
void ModelPrime(short dx, short dy, short pen);               /* one step, no output */
void ModelFull(short dx, short dy, short pen);                /* step + output + sample */
void ModelNext(void);                        /* step on the previous sample */
void ModelSample(short *dx, short *dy, short *pen);
extern long gModelBytes;
#endif
