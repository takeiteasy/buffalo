/*
 * t_dfa.c -- host unit tests for include/buffalo/buf_dfa.h.
 *
 * Plain `cc`, no cccc, but linked against runtime/buf_rt.c: the DFA is built
 * in-process from a `.bflo` spec, its four tables are handed straight to the
 * real `buf_run` driver, and the token stream is asserted. That is the M5
 * generated/native parity check arriving early -- it pins the table layout
 * (`next[state*nclass + cls[byte]]`, `cls` totality, the `%skip` / -1
 * convention) against the hand-written M0 reference in examples/digits_tables.c
 * and against t_nfa.c's independent NFA walk.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "buf_rx.h"
#include "buf_nfa.h"
#include "buf_dfa.h"
#include "buf_rt.h"

/* the local constant in buf_dfa.h must track the runtime enum */
typedef char assert_first_user_tok
    [(BUF_DFA_FIRST_USER_TOK == BUF_TOK_FIRST_USER) ? 1 : -1];

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, what)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));          \
        }                                                                    \
    } while (0)

static BufRx  rx;
static BufNfa nfa;
static BufDfa dfa;
static BufDfa dfa2;   /* second build, for the determinism check */
static BufDfa dfa_um; /* unminimised build, for the minimisation check */

static int build_string(const char *spec)
{
    int rc = buf_rx_parse_string(&rx, "<t>", spec);
    CHECK(rc == 0, "spec string parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return -1; }
    rc = buf_nfa_build(&nfa, &rx);
    CHECK(rc == 0, "NFA builds");
    if (rc != 0) { printf("  -> %s\n", nfa.error); return -1; }
    rc = buf_dfa_build(&dfa, &nfa, &rx);
    CHECK(rc == 0, "DFA builds");
    if (rc != 0) { printf("  -> %s\n", dfa.error); return -1; }
    return 0;
}

static int build(const char *path)
{
    int rc = buf_rx_read_file(&rx, path);
    CHECK(rc == 0, "spec parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return -1; }
    rc = buf_nfa_build(&nfa, &rx);
    CHECK(rc == 0, "NFA builds");
    if (rc != 0) { printf("  -> %s\n", nfa.error); return -1; }
    rc = buf_dfa_build(&dfa, &nfa, &rx);
    CHECK(rc == 0, "DFA builds");
    if (rc != 0) { printf("  -> %s\n", dfa.error); return -1; }
    return 0;
}

/* Run the real buf_run over `src` using an explicit dfa's tables. */
static int run_lex_on(const BufDfa *d, const char *src, int len,
                      int *kinds, int *lens, int max)
{
    BufLexer lx;
    int n = 0;
    buf_lexer_init(&lx, src, len);
    for (;;) {
        BufToken tk = buf_run(&lx, d->cls, d->next, d->accept,
                              d->rule_token, d->nstates, d->nclass, d->start);
        if (tk.kind == BUF_TOK_EOF) break;
        if (n >= max) break;
        kinds[n] = tk.kind;
        lens[n]  = tk.length;
        n++;
    }
    return n;
}

/* Run the real buf_run over `src` using the freshly built file-scope dfa. */
static int run_lex(const char *src, int len, int *kinds, int *lens, int max)
{
    return run_lex_on(&dfa, src, len, kinds, lens, max);
}

static void test_cls_total(void)
{
    int c, bad = 0;
    if (build("examples/calc.bflo") != 0) return;
    for (c = 0; c < 256; c++)
        if (dfa.cls[c] < 0 || dfa.cls[c] >= dfa.nclass) bad = 1;
    CHECK(!bad, "cls[] is total over 0..255, every entry in 0..nclass-1");
    CHECK(dfa.accept[dfa.start] < 0, "start state is non-accepting");
    CHECK(dfa.nclass < 32, "calc.bflo collapses to well under 32 classes");
}

static void test_digits_parity(void)
{
    int k[32], l[32], n;
    /* mirrors examples/digits_main.c expectations for the hand table */
    if (build("examples/digits.bflo") != 0) return;

    CHECK(dfa.rule_token[0] == BUF_TOK_FIRST_USER, "rule 0 -> TOK_INT value");
    CHECK(dfa.rule_token[1] == -1, "rule 1 (%skip) -> -1");

    n = run_lex("12 34\n5 x 6\n", (int)strlen("12 34\n5 x 6\n"), k, l, 32);
    CHECK(n == 5, "digits stream: 5 tokens");
    if (n == 5) {
        CHECK(k[0] == BUF_TOK_FIRST_USER && l[0] == 2, "INT '12'");
        CHECK(k[1] == BUF_TOK_FIRST_USER && l[1] == 2, "INT '34'");
        CHECK(k[2] == BUF_TOK_FIRST_USER && l[2] == 1, "INT '5'");
        CHECK(k[3] == BUF_TOK_ERROR     && l[3] == 1, "ERROR 'x'");
        CHECK(k[4] == BUF_TOK_FIRST_USER && l[4] == 1, "INT '6'");
    }
}

static void test_calc_stream(void)
{
    int k[32], l[32], n;
    if (build("examples/calc.bflo") != 0) return;

    /* %tokens INT FLOAT IDENT PLUS STAR LPAREN RPAREN -> values 2..8 */
    n = run_lex("1 + 2.5 * foo", (int)strlen("1 + 2.5 * foo"), k, l, 32);
    CHECK(n == 5, "calc stream: 5 tokens");
    if (n == 5) {
        CHECK(k[0] == 2 && l[0] == 1, "INT '1'");
        CHECK(k[1] == 5 && l[1] == 1, "PLUS");
        CHECK(k[2] == 3 && l[2] == 3, "FLOAT '2.5' (longest match beats INT '.'?)");
        CHECK(k[3] == 6 && l[3] == 1, "STAR");
        CHECK(k[4] == 4 && l[4] == 3, "IDENT 'foo'");
    }

    n = run_lex("1 # x\n2", (int)strlen("1 # x\n2"), k, l, 32);
    CHECK(n == 2 && k[0] == 2 && k[1] == 2, "'#' comment skipped");

    n = run_lex("$", 1, k, l, 32);
    CHECK(n == 1 && k[0] == BUF_TOK_ERROR, "stray '$' -> ERROR + resync");
}

/* An epsilon-cycle spec (nullable sub-expression, non-nullable rule) must
 * still build a finite DFA and drive buf_run without spinning. */
static void test_epsilon_cycle_dfa(void)
{
    int k[16], l[16], n;
    if (build_string("%tokens A\nA (a?)+b\n") != 0) return;
    CHECK(dfa.accept[dfa.start] < 0, "(a?)+b start state non-accepting");
    n = run_lex("aaab", 4, k, l, 16);
    CHECK(n == 1 && k[0] == BUF_TOK_FIRST_USER && l[0] == 4,
          "(a?)+b DFA matches 'aaab'");
    n = run_lex("b", 1, k, l, 16);
    CHECK(n == 1 && k[0] == BUF_TOK_FIRST_USER && l[0] == 1,
          "(a?)+b DFA matches 'b'");
}

static void test_clike(void)
{
    int k[64], l[64], n;
    if (build("examples/clike.bflo") != 0) return;

    CHECK(dfa.nclass < 64, "clike.bflo alphabet is far below 256 classes");
    CHECK(dfa.accept[dfa.start] < 0, "clike start state is non-accepting");

    /* KW_IF is %tokens slot 0 -> value 2; IDENT is slot 8 -> value 10 */
    n = run_lex("if x", 4, k, l, 64);
    CHECK(n == 2, "'if x' -> 2 tokens");
    if (n == 2) {
        CHECK(k[0] == 2 && l[0] == 2, "'if' is KW_IF, not IDENT (earlier rule wins tie)");
        CHECK(k[1] == 10 && l[1] == 1, "'x' is IDENT");
    }

    n = run_lex("iffy", 4, k, l, 64);
    CHECK(n == 1 && k[0] == 10 && l[0] == 4, "'iffy' is one IDENT (longest match)");

    /* M3 headroom: a real ~40-rule lexer must sit well inside every arena cap
     * so the placeholder sizes are not silently one bad spec from overflow.
     * `nfa` is the file-scope build scratch that build() just filled. */
    CHECK(rx.rule_count * 2 < BUF_RX_MAX_RULES,
          "clike rules < 1/2 of BUF_RX_MAX_RULES");
    CHECK(nfa.state_count * 4 < BUF_NFA_MAX_STATES,
          "clike NFA states < 1/4 of BUF_NFA_MAX_STATES");
    CHECK(dfa.nstates * 4 < BUF_DFA_MAX_STATES,
          "clike DFA states < 1/4 of BUF_DFA_MAX_STATES");
    CHECK(dfa.nstates * dfa.nclass * 4 < BUF_DFA_MAX_TRANS,
          "clike transition table < 1/4 of BUF_DFA_MAX_TRANS");
    CHECK(dfa.pool_used * 4 < BUF_DFA_SET_POOL,
          "clike state-set pool < 1/4 of BUF_DFA_SET_POOL");

    n = run_lex("a /* c */ b", (int)strlen("a /* c */ b"), k, l, 64);
    CHECK(n == 2 && k[0] == 10 && k[1] == 10, "block comment skipped");

    n = run_lex("a // trailing\nb", (int)strlen("a // trailing\nb"), k, l, 64);
    CHECK(n == 2 && k[0] == 10 && k[1] == 10, "line comment skipped");

    n = run_lex("x->y", 4, k, l, 64);
    CHECK(n == 3, "'x->y' -> 3 tokens");
    if (n == 3)
        CHECK(l[1] == 2, "'->' lexes as one 2-byte token (longest match over '-')");
}

/* The construction must be deterministic -- first-appearance class numbering,
 * discovery-order state ids, and (M3) a generation-stamped closure + hashed
 * state-set lookup that must not perturb either -- or the M5 generated/native
 * paths won't produce byte-identical tables. Build `path` twice and compare
 * the four tables. `big.bflo` (324 DFA states) is the regression guard for the
 * M3 DFA-construction rework. */
static void test_determinism_of(const char *path)
{
    static BufRx  rx_b;
    static BufNfa nfa_b;
    int i, ntrans;

    if (build(path) != 0) return;                       /* fills dfa */
    if (buf_rx_read_file(&rx_b, path) != 0) return;
    if (buf_nfa_build(&nfa_b, &rx_b) != 0) return;
    if (buf_dfa_build(&dfa2, &nfa_b, &rx_b) != 0) return;

    CHECK(dfa2.nclass == dfa.nclass && dfa2.nstates == dfa.nstates &&
          dfa2.nrules == dfa.nrules && dfa2.start == dfa.start,
          "second build agrees on nclass / nstates / nrules / start");

    for (i = 0; i < 256; i++)
        if (dfa2.cls[i] != dfa.cls[i]) break;
    CHECK(i == 256, "cls[] is byte-identical across builds");

    ntrans = dfa.nstates * dfa.nclass;
    for (i = 0; i < ntrans; i++)
        if (dfa2.next[i] != dfa.next[i]) break;
    CHECK(i == ntrans, "next[] is byte-identical across builds");

    for (i = 0; i < dfa.nstates; i++)
        if (dfa2.accept[i] != dfa.accept[i]) break;
    CHECK(i == dfa.nstates, "accept[] is byte-identical across builds");

    for (i = 0; i < dfa.nrules; i++)
        if (dfa2.rule_token[i] != dfa.rule_token[i]) break;
    CHECK(i == dfa.nrules, "rule_token[] is byte-identical across builds");
}

static void test_determinism(void)
{
    test_determinism_of("examples/clike.bflo");
    test_determinism_of("examples/json.bflo");
    test_determinism_of("examples/big.bflo");
}

/* Opt-in Moore minimisation (buf_dfa_minimize, gated by -D BUF_MINIMIZE in
 * the comptime build; here driven directly via buf_dfa_build_ex). It must
 * never change the recognised language or the winning rule -- so the token
 * stream through the minimised tables must match the unminimised ones
 * exactly -- and it must only ever remove states, deterministically. */
static void min_pair(const char *path, const char *src)
{
    int ku[128], lu[128], km[128], lm[128], nu, nm, i, slen = (int)strlen(src);

    if (buf_rx_read_file(&rx, path) != 0) { printf("  (skip %s)\n", path); return; }
    if (buf_nfa_build(&nfa, &rx) != 0)    { printf("  -> %s\n", nfa.error); return; }

    CHECK(buf_dfa_build_ex(&dfa_um, &nfa, &rx, 0) == 0, "unminimised DFA builds");
    CHECK(buf_dfa_build_ex(&dfa,    &nfa, &rx, 1) == 0, "minimised DFA builds");

    CHECK(dfa.nstates <= dfa_um.nstates, "minimisation never adds states");
    CHECK(dfa.nstates_premin == dfa_um.nstates,
          "nstates_premin records the pre-minimisation count");
    CHECK(dfa.accept[dfa.start] < 0, "minimised start state is non-accepting");
    CHECK(dfa.nclass == dfa_um.nclass && dfa.nrules == dfa_um.nrules,
          "minimisation leaves classes and rules alone");

    nu = run_lex_on(&dfa_um, src, slen, ku, lu, 128);
    nm = run_lex_on(&dfa,    src, slen, km, lm, 128);
    CHECK(nu == nm, "same token count before/after minimisation");
    if (nu == nm) {
        for (i = 0; i < nu; i++)
            if (ku[i] != km[i] || lu[i] != lm[i]) break;
        CHECK(i == nu, "identical (kind,length) stream before/after minimisation");
    }

    /* determinism of the minimised tables: build a second time, compare */
    {
        static BufRx  rx_b;
        static BufNfa nfa_b;
        int ntrans = dfa.nstates * dfa.nclass;
        if (buf_rx_read_file(&rx_b, path) == 0 &&
            buf_nfa_build(&nfa_b, &rx_b) == 0 &&
            buf_dfa_build_ex(&dfa2, &nfa_b, &rx_b, 1) == 0) {
            CHECK(dfa2.nstates == dfa.nstates && dfa2.start == dfa.start,
                  "minimised build is deterministic (nstates / start)");
            for (i = 0; i < ntrans; i++) if (dfa2.next[i] != dfa.next[i]) break;
            CHECK(i == ntrans, "minimised next[] is byte-identical across builds");
            for (i = 0; i < dfa.nstates; i++)
                if (dfa2.accept[i] != dfa.accept[i]) break;
            CHECK(i == dfa.nstates, "minimised accept[] is byte-identical across builds");
        }
    }
}

static void test_minimize(void)
{
    min_pair("examples/calc.bflo",  "1 + 2.5 * foo\n(a )# c\nbar");
    min_pair("examples/clike.bflo", "if (x->y) { return 42; } /* c */ // z\niffy");
    min_pair("examples/json.bflo",
             "{\"a\": [1, -2.5e1, true, false, null], \"b\": \"c\\u00e9\"}");
    min_pair("examples/big.bflo",
             "int x = 0xFF; float y = 3.14e2;\n"
             "if (x >= y) x <<= 1; // done\nreturn \"hi\";\n");

    /* a spec with provably redundant states: `abd` and `acd` share a tail --
     * the post-`ab` and post-`ac` states are non-accepting, both step to the
     * lone accept state on `d` and nowhere else, so they merge. */
    if (build_string("%tokens A\nA abd|acd\n") == 0) {
        int pre;
        if (buf_nfa_build(&nfa, &rx) == 0 &&
            buf_dfa_build_ex(&dfa_um, &nfa, &rx, 0) == 0 &&
            buf_dfa_build_ex(&dfa,    &nfa, &rx, 1) == 0) {
            int k[8], l[8], n;
            pre = dfa_um.nstates;
            CHECK(pre == 6, "abd|acd is 6 states unminimised");
            CHECK(dfa.nstates == 4, "abd|acd minimises to 4 states (tail merged)");
            n = run_lex_on(&dfa, "abd", 3, k, l, 8);
            CHECK(n == 1 && k[0] == BUF_TOK_FIRST_USER && l[0] == 3,
                  "minimised abd|acd still matches 'abd'");
            n = run_lex_on(&dfa, "acd", 3, k, l, 8);
            CHECK(n == 1 && k[0] == BUF_TOK_FIRST_USER && l[0] == 3,
                  "minimised abd|acd still matches 'acd'");
        }
    }
}

/* Native per-phase timing -- printed, never asserted (a slow machine must not
 * fail the suite). This is the M3 control for the comptime bench
 * (tests/bench.sh): if the native shape scales the same way the comptime VM's
 * does, the cost is algorithmic; if native is flat where the VM is not, the
 * cost is VM interpretation overhead and tuning the arenas will not move it.
 * See docs/performance.md. */
static void time_phases(const char *path)
{
    static BufRx  trx;
    static BufNfa tnfa;
    static BufDfa tdfa;
    const int     iters = 200;
    int           i;
    clock_t       t0;
    double        rd, nf, df, dm;

    if (buf_rx_read_file(&trx, path) != 0) { printf("  (skip %s)\n", path); return; }

    t0 = clock();
    for (i = 0; i < iters; i++) buf_rx_read_file(&trx, path);
    rd = (double)(clock() - t0) / CLOCKS_PER_SEC / iters * 1e3;

    t0 = clock();
    for (i = 0; i < iters; i++) buf_nfa_build(&tnfa, &trx);
    nf = (double)(clock() - t0) / CLOCKS_PER_SEC / iters * 1e3;

    t0 = clock();
    for (i = 0; i < iters; i++) buf_dfa_build_ex(&tdfa, &tnfa, &trx, 0);
    df = (double)(clock() - t0) / CLOCKS_PER_SEC / iters * 1e3;

    t0 = clock();
    for (i = 0; i < iters; i++) buf_dfa_build_ex(&tdfa, &tnfa, &trx, 1);
    dm = (double)(clock() - t0) / CLOCKS_PER_SEC / iters * 1e3;

    printf("  %-18s read %7.3f ms   nfa %7.3f ms   dfa %7.3f ms   dfa+min %7.3f ms\n",
           path, rd, nf, df, dm);
}

int main(void)
{
    test_cls_total();
    test_digits_parity();
    test_calc_stream();
    test_epsilon_cycle_dfa();
    test_clike();
    test_determinism();
    test_minimize();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "ok  ", checks, failures);

    printf("native per-phase timing (host cc, not cccc):\n");
    time_phases("examples/calc.bflo");
    time_phases("examples/clike.bflo");
    time_phases("examples/big.bflo");

    return failures ? 1 : 0;
}
