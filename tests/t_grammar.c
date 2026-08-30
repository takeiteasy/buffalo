/*
 * t_grammar.c -- host unit tests for include/buffalo/buf_grammar.h.
 *
 * Plain `cc`, no cccc. Builds LALR(1) tables from a `.bflo` spec's %grammar
 * section and checks them three ways: structural invariants over the raw
 * action/goto tables, a handful of spot-checked entries, and a throwaway
 * ~15-line shift/reduce/accept driver (the t_nfa.c-style independent-walker
 * role) fed calc.bflo's real DFA token stream -- NOT buf_parse/ticket #23's
 * driver, just enough to prove the tables actually recognise the language.
 */
#include <stdio.h>
#include <string.h>

#include "buf_rx.h"
#include "buf_nfa.h"
#include "buf_dfa.h"
#include "buf_grammar.h"
#include "buf_rt.h"

/* the local constant in buf_grammar.h must track the runtime enum, and
 * buf_dfa.h's own copy of the same convention. */
typedef char assert_first_user_tok_grammar
    [(BUF_LALR_FIRST_USER_TOK == BUF_TOK_FIRST_USER) ? 1 : -1];
typedef char assert_first_user_tok_dfa_grammar_agree
    [(BUF_LALR_FIRST_USER_TOK == BUF_DFA_FIRST_USER_TOK) ? 1 : -1];

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

static BufRx      rx;
static BufNfa      nfa;
static BufDfa      dfa;
static BufGrammar  g;
static BufGrammar  g2; /* second build, for the determinism check */

static int build(const char *path) {
    int rc = buf_rx_read_file(&rx, path);
    CHECK(rc == 0, "spec parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return -1; }
    rc = buf_grammar_build(&g, &rx);
    CHECK(rc == 0, "grammar builds");
    if (rc != 0) { printf("  -> %s\n", g.error); return -1; }
    return 0;
}

static int build_string(const char *spec) {
    int rc = buf_rx_parse_string(&rx, "<t>", spec);
    CHECK(rc == 0, "spec string parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return -1; }
    rc = buf_grammar_build(&g, &rx);
    CHECK(rc == 0, "grammar builds");
    if (rc != 0) { printf("  -> %s\n", g.error); return -1; }
    return 0;
}

/* Expect buf_grammar_build to fail; return the error string (or NULL). */
static const char *build_string_expect_fail(const char *spec) {
    int rc = buf_rx_parse_string(&rx, "<t>", spec);
    if (rc != 0) return rx.error;
    rc = buf_grammar_build(&g, &rx);
    CHECK(rc == -1, "grammar build is rejected");
    return rc == -1 ? g.error : NULL;
}

/* --- structural invariants ---------------------------------------------- */

static void test_calc_structure(void) {
    int s, t, every_state_has_action = 1, accepts = 0;

    if (build("examples/calc.bflo") != 0) return;

    /* pinned regression value: calc.bflo's expr/term/factor grammar */
    CHECK(g.nstates == 14, "calc.bflo LALR(1) automaton has 14 states");

    for (s = 0; s < g.nstates; s++) {
        int has_any = 0;
        for (t = 0; t < g.ntok; t++) {
            int v = g.action[s * g.ntok + t];
            if (v != BUF_LALR_ACT_ERROR) has_any = 1;
            if (v == BUF_LALR_ACT_ACCEPT) accepts++;
        }
        if (!has_any) every_state_has_action = 0;
    }
    CHECK(every_state_has_action, "every state has at least one action entry");
    CHECK(accepts == 1, "the accept sentinel appears exactly once");

    /* goto entries only ever land on nonterminal columns -- trivially true
     * by construction (goto_tab is indexed by nonterminal only), so instead
     * check every populated goto cell names a real state. */
    {
        int bad = 0, nt;
        for (s = 0; s < g.nstates; s++)
            for (nt = 0; nt < g.nnonterm; nt++) {
                int v = g.goto_tab[s * g.nnonterm + nt];
                if (v != -1 && (v < 0 || v >= g.nstates)) bad = 1;
            }
        CHECK(!bad, "every populated goto cell names a valid state");
    }
}

static void test_calc_spot_checks(void) {
    int tINT, tFLOAT, tIDENT, tPLUS, tSTAR, tLPAREN, tRPAREN;
    int ntExpr, ntTerm, ntFactor;
    int s0;

    if (build("examples/calc.bflo") != 0) return;

    tINT    = buf_rx_token_index(&rx, "INT");
    tFLOAT  = buf_rx_token_index(&rx, "FLOAT");
    tIDENT  = buf_rx_token_index(&rx, "IDENT");
    tPLUS   = buf_rx_token_index(&rx, "PLUS");
    tSTAR   = buf_rx_token_index(&rx, "STAR");
    tLPAREN = buf_rx_token_index(&rx, "LPAREN");
    tRPAREN = buf_rx_token_index(&rx, "RPAREN");
    ntExpr   = buf_rx_nt_index(&rx, "expr");
    ntTerm   = buf_rx_nt_index(&rx, "term");
    ntFactor = buf_rx_nt_index(&rx, "factor");
    CHECK(tINT >= 0 && tFLOAT >= 0 && tIDENT >= 0 && tPLUS >= 0 &&
          tSTAR >= 0 && tLPAREN >= 0 && tRPAREN >= 0, "calc tokens resolve");
    CHECK(ntExpr >= 0 && ntTerm >= 0 && ntFactor >= 0, "calc nonterms resolve");

    s0 = g.start_state;
    CHECK(BUF_LALR_IS_SHIFT(g.action[s0 * g.ntok + tINT + BUF_LALR_FIRST_USER_TOK]),
          "start state shifts on INT");
    CHECK(BUF_LALR_IS_SHIFT(g.action[s0 * g.ntok + tFLOAT + BUF_LALR_FIRST_USER_TOK]),
          "start state shifts on FLOAT");
    CHECK(BUF_LALR_IS_SHIFT(g.action[s0 * g.ntok + tIDENT + BUF_LALR_FIRST_USER_TOK]),
          "start state shifts on IDENT");
    CHECK(BUF_LALR_IS_SHIFT(g.action[s0 * g.ntok + tLPAREN + BUF_LALR_FIRST_USER_TOK]),
          "start state shifts on LPAREN");
    CHECK(g.goto_tab[s0 * g.nnonterm + ntExpr] != -1, "start state gotos on expr");
    CHECK(g.goto_tab[s0 * g.nnonterm + ntTerm] != -1, "start state gotos on term");
    CHECK(g.goto_tab[s0 * g.nnonterm + ntFactor] != -1, "start state gotos on factor");

    /* the state reached after shifting INT from the start state must reduce
     * factor -> INT on every lookahead that can follow a factor. */
    {
        int s1 = BUF_LALR_SHIFT_STATE(
            g.action[s0 * g.ntok + tINT + BUF_LALR_FIRST_USER_TOK]);
        int v_star = g.action[s1 * g.ntok + tSTAR + BUF_LALR_FIRST_USER_TOK];
        int v_plus = g.action[s1 * g.ntok + tPLUS + BUF_LALR_FIRST_USER_TOK];
        int v_eof  = g.action[s1 * g.ntok + 0];
        CHECK(BUF_LALR_IS_REDUCE(v_star) && BUF_LALR_IS_REDUCE(v_plus) &&
              BUF_LALR_IS_REDUCE(v_eof),
              "post-INT state reduces 'factor : INT' on STAR/PLUS/EOF");
    }

    /* the state reached after shifting expr, then PLUS, then term (i.e.
     * after a full 'expr PLUS term' recognised) must shift STAR (term can
     * grow) and reduce on PLUS/EOF/RPAREN (expr : expr PLUS term wins). */
    {
        int s_expr = g.goto_tab[s0 * g.nnonterm + ntExpr];
        int s_plus = BUF_LALR_SHIFT_STATE(
            g.action[s_expr * g.ntok + tPLUS + BUF_LALR_FIRST_USER_TOK]);
        int s_term = g.goto_tab[s_plus * g.nnonterm + ntTerm];
        CHECK(BUF_LALR_IS_SHIFT(g.action[s_term * g.ntok + tSTAR + BUF_LALR_FIRST_USER_TOK]),
              "post-'expr PLUS term' state still shifts STAR");
        CHECK(BUF_LALR_IS_REDUCE(g.action[s_term * g.ntok + 0]),
              "post-'expr PLUS term' state reduces on EOF");
        CHECK(BUF_LALR_IS_REDUCE(g.action[s_term * g.ntok + tRPAREN + BUF_LALR_FIRST_USER_TOK]),
              "post-'expr PLUS term' state reduces on RPAREN");
    }
}

/* --- reference LR driver: explicit stack, decode, shift/reduce/accept --- */

static int lr_accepts(BufGrammar *gm, BufRx *rx_, BufDfa *dfa_, const char *src) {
    int stack[256];
    int sp = 0;
    BufLexer lx;
    BufToken tk;

    buf_lexer_init(&lx, src, (int)strlen(src));
    stack[sp++] = gm->start_state;
    tk = buf_run(&lx, dfa_->cls, dfa_->next, dfa_->accept, dfa_->rule_token,
                dfa_->nstates, dfa_->nclass, dfa_->start);
    for (;;) {
        int st = stack[sp - 1];
        int v  = gm->action[st * gm->ntok + tk.kind];
        if (v == BUF_LALR_ACT_ERROR) return 0;
        if (v == BUF_LALR_ACT_ACCEPT) return 1;
        if (BUF_LALR_IS_SHIFT(v)) {
            if (sp >= 256) return 0;
            stack[sp++] = BUF_LALR_SHIFT_STATE(v);
            tk = buf_run(&lx, dfa_->cls, dfa_->next, dfa_->accept,
                        dfa_->rule_token, dfa_->nstates, dfa_->nclass, dfa_->start);
        } else {
            int p = BUF_LALR_REDUCE_PROD(v);
            int lhs = buf_lalr_plhs(rx_, p), len = buf_lalr_plen(rx_, p), i;
            for (i = 0; i < len; i++) sp--;
            if (sp <= 0) return 0;
            {
                int gs = gm->goto_tab[stack[sp - 1] * gm->nnonterm + lhs];
                stack[sp++] = gs;
            }
        }
    }
}

static void test_calc_reference_driver(void) {
    if (build("examples/calc.bflo") != 0) return;
    if (buf_nfa_build(&nfa, &rx) != 0) { printf("  -> %s\n", nfa.error); return; }
    if (buf_dfa_build(&dfa, &nfa, &rx) != 0) { printf("  -> %s\n", dfa.error); return; }

    CHECK(lr_accepts(&g, &rx, &dfa, "1+2*3"), "'1+2*3' accepts");
    CHECK(lr_accepts(&g, &rx, &dfa, "(1+2)*x"), "'(1+2)*x' accepts");
    CHECK(!lr_accepts(&g, &rx, &dfa, "1+"), "'1+' rejects");
    CHECK(!lr_accepts(&g, &rx, &dfa, "(1"), "'(1' rejects");
    CHECK(!lr_accepts(&g, &rx, &dfa, "1 2"), "'1 2' rejects");
}

/* --- determinism --------------------------------------------------------- */

static void test_determinism(void) {
    static BufRx rx_b;
    int i, na, ng;

    if (build("examples/calc.bflo") != 0) return;
    if (buf_rx_read_file(&rx_b, "examples/calc.bflo") != 0) return;
    if (buf_grammar_build(&g2, &rx_b) != 0) { printf("  -> %s\n", g2.error); return; }

    CHECK(g2.nstates == g.nstates && g2.ntok == g.ntok &&
          g2.nnonterm == g.nnonterm && g2.start_state == g.start_state,
          "second build agrees on nstates/ntok/nnonterm/start_state");

    na = g.nstates * g.ntok;
    for (i = 0; i < na; i++) if (g2.action[i] != g.action[i]) break;
    CHECK(i == na, "action[] is byte-identical across builds");

    ng = g.nstates * g.nnonterm;
    for (i = 0; i < ng; i++) if (g2.goto_tab[i] != g.goto_tab[i]) break;
    CHECK(i == ng, "goto_tab[] is byte-identical across builds");
}

/* --- epsilon production -------------------------------------------------- */

static void test_epsilon_production(void) {
    /* args is nullable: 'args : | INT args ;' i.e. a possibly-empty list of
     * INTs, right-recursive so it exercises an epsilon reduce item. */
    if (build_string(
            "%tokens INT\nINT [0-9]+\n%skip [ \\t\\r\\n]+\n"
            "%grammar\n%start args\nargs : | INT args ;\n") != 0)
        return;
    CHECK(g.nstates > 0, "epsilon-production grammar builds a non-trivial automaton");
}

/* --- deeper state chains: a 5-tier precedence ladder ---------------------
 *
 * Structural signal only (tables build, no conflicts, a plausible state
 * count) -- exercising layered-nonterminal precedence more deeply than
 * calc.bflo's 3 tiers without committing to a full example file + fixtures
 * before ticket #23 has a runtime driver to actually run one against.
 */
static void test_precedence_ladder(void) {
    if (build_string(
            "%tokens INT OROR ANDAND EQ PLUS STAR LPAREN RPAREN\n"
            "INT [0-9]+\nOROR \"||\"\nANDAND \"&&\"\nEQ \"==\"\nPLUS \"+\"\n"
            "STAR \"*\"\nLPAREN \"(\"\nRPAREN \")\"\n%skip [ \\t\\r\\n]+\n"
            "%grammar\n%start e1\n"
            "e1 : e1 OROR e2 | e2 ;\n"
            "e2 : e2 ANDAND e3 | e3 ;\n"
            "e3 : e3 EQ e4 | e4 ;\n"
            "e4 : e4 PLUS e5 | e5 ;\n"
            "e5 : e5 STAR f | f ;\n"
            "f  : LPAREN e1 RPAREN | INT ;\n") != 0)
        return;
    CHECK(g.nstates > 14, "5-tier precedence ladder needs more states than calc.bflo");
}

/* --- conflict detection --------------------------------------------------- */

static void test_shift_reduce_conflict(void) {
    const char *err = build_string_expect_fail(
        "%tokens INT PLUS\nINT [0-9]+\nPLUS \"+\"\n%skip [ \\t\\r\\n]+\n"
        "%grammar\n%start e\ne : e PLUS e | INT ;\n");
    CHECK(err != NULL && strstr(err, "shift/reduce") != NULL,
          "ambiguous left/right recursion is a shift/reduce conflict");
    CHECK(err != NULL && strstr(err, "PLUS") != NULL,
          "conflict error names the conflicting token 'PLUS'");
}

static void test_reduce_reduce_conflict(void) {
    const char *err = build_string_expect_fail(
        "%tokens INT\nINT [0-9]+\n%skip [ \\t\\r\\n]+\n"
        "%grammar\n%start s\ns : a | b ;\na : INT ;\nb : INT ;\n");
    CHECK(err != NULL && strstr(err, "reduce/reduce") != NULL,
          "s : a | b with a,b both -> INT is a reduce/reduce conflict");
    CHECK(err != NULL && strstr(err, "'a'") != NULL && strstr(err, "'b'") != NULL,
          "conflict error names both 'a' and 'b'");
}

/* --- validation: non-productive / unreachable ---------------------------- */

static void test_non_productive(void) {
    const char *err = build_string_expect_fail(
        "%tokens INT\nINT [0-9]+\n%skip [ \\t\\r\\n]+\n"
        "%grammar\n%start s\ns : INT | loop ;\nloop : loop INT ;\n");
    CHECK(err != NULL && strstr(err, "non-productive") != NULL,
          "'loop : loop INT' (no base case) is non-productive");
}

static void test_unreachable(void) {
    const char *err = build_string_expect_fail(
        "%tokens INT\nINT [0-9]+\n%skip [ \\t\\r\\n]+\n"
        "%grammar\n%start s\ns : INT ;\ndead : INT ;\n");
    CHECK(err != NULL && strstr(err, "unreachable") != NULL,
          "'dead', never referenced from %start, is unreachable");
}

int main(void) {
    test_calc_structure();
    test_calc_spot_checks();
    test_calc_reference_driver();
    test_determinism();
    test_epsilon_production();
    test_precedence_ladder();
    test_shift_reduce_conflict();
    test_reduce_reduce_conflict();
    test_non_productive();
    test_unreachable();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "ok  ", checks, failures);
    return failures ? 1 : 0;
}
