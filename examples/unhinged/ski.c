#include <stdio.h>
#include <stdlib.h>

/* Arithmetic with nothing but the S, K and I combinators. Numbers are Church
 * numerals built from them, sums and products and powers are applications,
 * and the only way to read an answer back is to apply the numeral to an
 * increment and a zero and let graph reduction run. */

enum { T_S, T_K, T_I, T_APPLY, T_INCREMENT, T_NUMBER };

typedef struct { int kind, left, right; long value; } Term;

static Term terms[400000];
static int term_count, spine[100000];
static long reductions;

static int term(int kind, int left, int right, long value) {
    if (term_count == (int)(sizeof terms / sizeof terms[0])) { fprintf(stderr, "ski: out of terms\n"); exit(1); }
    terms[term_count] = (Term){kind, left, right, value};
    return term_count++;
}

static int S(void) { return term(T_S, 0, 0, 0); }
static int K(void) { return term(T_K, 0, 0, 0); }
static int I(void) { return term(T_I, 0, 0, 0); }
static int apply(int function, int argument) { return term(T_APPLY, function, argument, 0); }

/* B = S (K S) K composes: B f g x = f (g x). */
static int compose(void) { return apply(apply(S(), apply(K(), S())), K()); }
static int zero(void) { return apply(K(), I()); }
static int successor(void) { return apply(S(), compose()); }

static int church(int n) {
    int numeral = zero();
    while (n--) numeral = apply(successor(), numeral);
    return numeral;
}

static int plus(int m, int n) { return apply(apply(m, successor()), n); }
static int times(int m, int n) { return apply(apply(compose(), m), n); }
static int power(int base, int exponent) { return apply(exponent, base); }

/* Reduces to weak head normal form, overwriting each redex with its result so
 * that shared subterms are reduced once. */
static void reduce(int root) {
    for (;;) {
        int depth = 0, head = root;
        while (terms[head].kind == T_APPLY) {
            spine[depth++] = head;
            head = terms[head].left;
        }
        int kind = terms[head].kind;
        int needed = kind == T_S ? 3 : kind == T_K ? 2 : kind == T_I || kind == T_INCREMENT ? 1 : 0;
        if (!needed || depth < needed) return;
        int redex = spine[depth - needed];
        int x = terms[spine[depth - 1]].right;
        ++reductions;
        if (kind == T_I) terms[redex] = terms[x];
        else if (kind == T_K) terms[redex] = terms[x];
        else if (kind == T_S) {
            int y = terms[spine[depth - 2]].right, z = terms[redex].right;
            terms[redex] = (Term){T_APPLY, apply(x, z), apply(y, z), 0};
        } else {
            reduce(x);
            if (terms[x].kind != T_NUMBER) { fprintf(stderr, "ski: that was not a numeral\n"); exit(1); }
            terms[redex] = (Term){T_NUMBER, 0, 0, terms[x].value + 1};
        }
    }
}

static long decode(int numeral) {
    int root = apply(apply(numeral, term(T_INCREMENT, 0, 0, 0)), term(T_NUMBER, 0, 0, 0));
    reduce(root);
    return terms[root].value;
}

static void show(int t) {
    switch (terms[t].kind) {
        case T_S: putchar('S'); break;
        case T_K: putchar('K'); break;
        case T_I: putchar('I'); break;
        default:
            show(terms[t].left);
            if (terms[terms[t].right].kind == T_APPLY) { putchar('('); show(terms[t].right); putchar(')'); }
            else show(terms[t].right);
    }
}

static void report(const char *question, int expression) {
    int before = term_count;
    long before_reductions = reductions;
    long answer = decode(expression);
    printf("%-14s = %-4ld (%ld reductions, %d terms grown)\n", question, answer,
           reductions - before_reductions, term_count - before);
}

int main(void) {
    printf("two, written out: ");
    show(church(2));
    printf("\n\n");
    report("2 + 3", plus(church(2), church(3)));
    report("(2 + 3) * 4", times(plus(church(2), church(3)), church(4)));
    report("2 ^ 3", power(church(2), church(3)));
    report("3 ^ 3", power(church(3), church(3)));
    report("2 ^ (2 ^ 2)", power(church(2), power(church(2), church(2))));
    report("(3 * 3) ^ 2", power(times(church(3), church(3)), church(2)));
    printf("\n%d terms built, %ld reductions performed, and not one number was harmed.\n", term_count, reductions);
    return 0;
}
