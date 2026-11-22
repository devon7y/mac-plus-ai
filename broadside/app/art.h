#ifndef BS_ART_H
#define BS_ART_H
enum { ART_FLAME_A, ART_FLAME_B, ART_WRECK, ART_EXP1, ART_EXP2, ART_EXP3, ART_SPL1, ART_SPL2, ART_SPL3, ART_COUNT };
void art_init(void);
void art_draw(int a, short cx, short cy);
void art_draw_face(int mood, const Rect *r);
#endif
