/* test_dec.c - host test driver for dec.c (driven by test_dec.py; see its header).
 *
 * Reads one command per line on stdin, prints one result line per command:
 *   B <op> <s> <m> <e> <s> <m> <e>   op in a s m d p (add sub mul div pow) -> "<rc> <s> <m> <e>"
 *   N <point_pos> <digits>           dec_from_digits                      -> "<rc> <s> <m> <e>"
 *   F <sig> <s> <m> <e>              dec_format                           -> "<sci> <exp10> <text>"
 *   T <s> <m> <e>                    dec_to_text                          -> "<text>"
 *   E <tokens>                       dec_eval -> "<rc> <errpos> <s> <m> <e> <sci> <exp10> <fmt12> <text>"
 * Tokens: 0-9 + - * / ( ) . ^, '=' for T_EQ, '?' for an invalid token value (99).
 * Results with rc != 0 print zeros for the number fields.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dec.h"

static char line[1 << 16];
static unsigned char buf[1 << 16];

static char *next(char **s)
{
    char *t;
    while (**s == ' ') (*s)++;
    t = *s;
    while (**s && **s != ' ' && **s != '\n') (*s)++;
    if (**s) *(*s)++ = 0;
    return t;
}

static void rd(char **s, Dec *d)
{
    d->neg = (unsigned char)atoi(next(s));
    d->m = strtoull(next(s), NULL, 10);
    d->e = (short)atoi(next(s));
    d->pad = 0;
}

static void pr(int rc, const Dec *d)
{
    if (rc) printf("%d 0 0 0", rc);
    else printf("%d %d %llu %d", rc, d->neg, d->m, d->e);
}

int main(void)
{
    while (fgets(line, sizeof line, stdin)) {
        char *s = line + 1, *t, out[DEC_BUFLEN], txt[DEC_BUFLEN];
        Dec a, b, r;
        int rc = 0, sci, x, i, n, ep;
        switch (line[0]) {
        case 'B': {
            char op = *next(&s);
            rd(&s, &a); rd(&s, &b);
            memset(&r, 0x5a, sizeof r);
            switch (op) {
            case 'a': rc = dec_add(&r, &a, &b); break;
            case 's': rc = dec_sub(&r, &a, &b); break;
            case 'm': rc = dec_mul(&r, &a, &b); break;
            case 'd': rc = dec_div(&r, &a, &b); break;
            case 'p': rc = dec_pow(&r, &a, &b); break;
            }
            pr(rc, &r); putchar('\n');
            break;
        }
        case 'N':
            x = atoi(next(&s));
            t = next(&s);
            n = (int)strlen(t);
            for (i = 0; i < n; i++) buf[i] = (unsigned char)(t[i] - '0');
            rc = dec_from_digits(&r, buf, n, x);
            pr(rc, &r); putchar('\n');
            break;
        case 'F':
            x = atoi(next(&s));
            rd(&s, &a);
            n = dec_format(&a, x, out, &sci, &i);
            if (n != (int)strlen(out)) printf("LENGTH MISMATCH ");
            printf("%d %d %s\n", sci, i, out);
            break;
        case 'T':
            rd(&s, &a);
            n = dec_to_text(&a, out);
            if (n != (int)strlen(out)) printf("LENGTH MISMATCH ");
            printf("%s\n", out);
            break;
        case 'E':
            if (*s == ' ') s++;
            n = 0;
            for (; *s && *s != '\n'; s++) {
                int c = *s, v;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c == '+') v = T_PLUS;
                else if (c == '-') v = T_MINUS;
                else if (c == '*') v = T_TIMES;
                else if (c == '/') v = T_DIV;
                else if (c == '=') v = T_EQ;
                else if (c == '(') v = T_LPAR;
                else if (c == ')') v = T_RPAR;
                else if (c == '.') v = T_POINT;
                else if (c == '^') v = T_POW;
                else v = 99;
                buf[n++] = (unsigned char)v;
            }
            memset(&r, 0x5a, sizeof r);
            rc = dec_eval(buf, n, &r, &ep);
            printf("%d %d %d %llu %d", rc, ep, r.neg, r.m, r.e);
            n = dec_format(&r, 12, out, &sci, &x);
            dec_to_text(&r, txt);
            printf(" %d %d %s %s\n", sci, x, out, txt);
            break;
        default:
            printf("?\n");
        }
    }
    return 0;
}
