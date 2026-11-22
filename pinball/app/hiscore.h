#ifndef PB_HISCORE_H
#define PB_HISCORE_H
#define NSCORES 5
#define SCORES_VERSION 0x4E500001L      /* 'NP' v1 */
typedef struct { long score; char init[4]; } ScoreEntry;
typedef struct { long version; short sound, speech; ScoreEntry e[NSCORES]; } Scores;
extern Scores scores;
void scores_load(void);
void scores_save(void);
short scores_rank(long score);          /* 0..NSCORES-1, or -1 */
void scores_insert(short rank, long score, const char *init);
#endif
