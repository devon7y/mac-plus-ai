/* dec.h - decimal floating point and expression evaluator for Scribble Calc.
 *
 * Software decimal arithmetic for the 68000 (no FPU, no libc, no malloc, no floating point).
 * A Dec holds  (-1)^neg * m * 10^e  with 0 <= m < 10^18 (18 significant digits).
 *
 * dec_add, dec_sub, dec_mul, dec_div and dec_from_digits give exactly the result of Python's
 *     decimal.Context(prec=18, rounding=ROUND_HALF_EVEN, Emax=999, Emin=-999)
 * (correctly rounded, half-even, including Python's gradual underflow: results below 1E-999
 * keep fewer digits, down to an exponent of DEC_ETINY = -1016, and smaller results become 0).
 * The coefficient m is not normalised: 1.50 may be m=150,e=-2 or m=15,e=-1; compare values,
 * not fields. Zero is always m=0, e=0, neg=0 (no negative zero). There is no NaN or infinity:
 * every function that can fail returns an error code and leaves *r unspecified on failure.
 * All functions accept aliased arguments (r == a, r == b).
 */
#ifndef DEC_H
#define DEC_H

typedef struct {
    unsigned long long m;   /* coefficient, 0 <= m < 10^18 */
    short e;                /* exponent, DEC_ETINY <= e; e + digits(m) - 1 <= DEC_EMAX */
    unsigned char neg;      /* 1 = negative (never set for zero) */
    unsigned char pad;
} Dec;

#define DEC_PREC 18
#define DEC_EMAX 999                            /* largest |x| is 9.99...E999 */
#define DEC_EMIN (-999)                         /* smallest full-precision |x| is 1E-999 */
#define DEC_ETINY (DEC_EMIN - DEC_PREC + 1)     /* smallest |x| is 1E-1016 */
#define DEC_MAXDEPTH 32                         /* dec_eval nesting limit (unary frames) */
#define DEC_BUFLEN 32                           /* enough for dec_format and dec_to_text */

enum { DEC_OK = 0, DEC_DIV0, DEC_OVERFLOW, DEC_DOMAIN, DEC_SYNTAX };

/* digits[] are values 0..9; point_pos = number of digits before the decimal point (n if none;
   any int is accepted, e.g. -2 means ".00ddd"). Leading zeros are not significant; more than
   18 significant digits are rounded half-even. DEC_SYNTAX if n <= 0 or a digit is > 9,
   DEC_OVERFLOW if the value is above 9.99...E999. */
int dec_from_digits(Dec *out, const unsigned char *digits, int n, int point_pos);

int dec_add(Dec *r, const Dec *a, const Dec *b);
int dec_sub(Dec *r, const Dec *a, const Dec *b);
int dec_mul(Dec *r, const Dec *a, const Dec *b);
int dec_div(Dec *r, const Dec *a, const Dec *b);    /* DEC_DIV0 when b == 0 (also 0/0) */

/* base^exponent. The exponent must be an integer (after removing trailing zeros) with
   |n| <= 9999, else DEC_DOMAIN. 0^0 = 1; 0^negative = DEC_DIV0.
   Algorithm (NOT correctly rounded; error up to a few units in the 18th digit):
     res = 1; p = base; k = |n|;
     loop { if (k odd) res = res*p (rounded); k >>= 1; if (!k) break; p = p*p (rounded); }
     n < 0: res = 1/res (rounded).
   For n < 0: if base^|n| overflows the result is 0 (it underflows); if base^|n| underflows
   to 0 the result is DEC_OVERFLOW. */
int dec_pow(Dec *r, const Dec *base, const Dec *exponent);

void dec_neg(Dec *r, const Dec *a);
int dec_is_zero(const Dec *a);

/* Display form: round to sig (1..18) significant digits half-up (away from zero on .5, like a
   pocket calculator), strip trailing zeros and a trailing '.', then
     fixed notation when the rounded value is 0 or 1e-6 <= |x| < 1e12 ("-1234.5", "0.000123");
     otherwise scientific: buf gets the mantissa ("-1.23456789012"), *sci = 1, *exp10 = power
     of ten (e.g. 15).
   NUL-terminated ASCII ('-' minus, '.' point); returns the length. buf needs DEC_BUFLEN bytes.
   *sci = *exp10 = 0 for fixed notation. */
int dec_format(const Dec *a, int sig, char *buf, int *sci, int *exp10);

/* One-line clipboard form with 12 significant digits: "1234.5", "1.5E15", "1.5E-7".
   buf needs DEC_BUFLEN bytes; returns the length. */
int dec_to_text(const Dec *a, char *buf);

/* Tokens from the recogniser. */
enum {
    T_0 = 0, T_1, T_2, T_3, T_4, T_5, T_6, T_7, T_8, T_9 = 9,
    T_PLUS = 10, T_MINUS = 11, T_TIMES = 12, T_DIV = 13, T_EQ = 14,
    T_LPAR = 15, T_RPAR = 16, T_POINT = 17, T_POW = 18
};

/* Evaluate tok[0..n) (without the trailing T_EQ).
     expr    := term (('+'|'-') term)*
     term    := unary (('x'|'/') unary | <implicit x before '('> unary)*
     unary   := '-' unary | primary ('^' unary)?       (-2^2 = -4, 2^3^2 = 512, 2^-1 = 0.5)
     primary := number | '(' expr ')'
     number  := digits ['.' digits] | digits '.' | '.' digits
   The whole input is parsed first: any syntax problem (including nesting deeper than
   DEC_MAXDEPTH unary levels) gives DEC_SYNTAX with *err_pos = token index where it was found
   (n = unexpected end). Otherwise the first evaluation error in evaluation order (left operand,
   right operand, then the operator) is returned: DEC_DIV0, DEC_OVERFLOW or DEC_DOMAIN.
   *err_pos = -1 when there is no syntax error; err_pos may be NULL. On any error *result = 0. */
int dec_eval(const unsigned char *tok, int n, Dec *result, int *err_pos);

#endif
