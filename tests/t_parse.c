/*
 * t_parse.c -- host unit tests for runtime/buf_rt.h's buf_parse (#23), the
 * runtime LALR(1) driver that builds a concrete syntax tree over the
 * action/goto tables include/buffalo/buf_grammar.h builds (#22).
 *
 * Plain `cc`, no cccc. Builds tables from a `.bflo` spec's %grammar section
 * the same way tests/t_grammar.c does, bakes the prod_lhs[]/prod_len[]
 * flat arrays buf_parse needs (it cannot take a BufRx*, a comptime-only
 * type), and drives buf_parse over calc.bflo's real DFA/LALR tables --
 * generalizing t_grammar.c's throwaway lr_accepts() into a tree-builder
 * whose shape can actually be checked.
 */
#include <stdio.h>
#include <string.h>

#include "buf_rx.h"
#include "buf_nfa.h"
#include "buf_dfa.h"
#include "buf_grammar.h"
#include "buf_rt.h"

/* buf_rt.h's local BUF_PARSE_ACT_* copy must track buf_grammar.h's
 * BUF_LALR_ACT_* exactly -- same cross-check pattern as the FIRST_USER_TOK
 * triple in t_grammar.c/t_dfa.c. */
typedef char assert_parse_act_error_agrees
    [(BUF_PARSE_ACT_ERROR == BUF_LALR_ACT_ERROR) ? 1 : -1];
typedef char assert_parse_act_accept_agrees
    [(BUF_PARSE_ACT_ACCEPT == BUF_LALR_ACT_ACCEPT) ? 1 : -1];

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));         \
        }                                                                   \
    } while (0)

static BufRx     rx;
static BufNfa    nfa;
static BufDfa    dfa;
static BufGrammar g;

static int prod_lhs[BUF_RX_MAX_PRODS + 1];
static int prod_len[BUF_RX_MAX_PRODS + 1];

#define NODE_CAP  256
#define CHILD_CAP 512
#define STACK_CAP 128

static BufCstNode nodes[NODE_CAP];
static int        child[CHILD_CAP];
static int        state_stack[STACK_CAP];
static int        node_stack[STACK_CAP];

static int build(const char *path) {
    int rc = buf_rx_read_file(&rx, path);
    CHECK(rc == 0, "spec parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return -1; }
    rc = buf_grammar_build(&g, &rx);
    CHECK(rc == 0, "grammar builds");
    if (rc != 0) { printf("  -> %s\n", g.error); return -1; }
    if (buf_nfa_build(&nfa, &rx) != 0) { printf("  -> %s\n", nfa.error); return -1; }
    if (buf_dfa_build(&dfa, &nfa, &rx) != 0) { printf("  -> %s\n", dfa.error); return -1; }
    {
        int p;
        for (p = 0; p < rx.prod_count; p++) {
            prod_lhs[p] = buf_lalr_plhs(&rx, p);
            prod_len[p] = buf_lalr_plen(&rx, p);
        }
    }
    return 0;
}

static int build_string(const char *spec) {
    int rc = buf_rx_parse_string(&rx, "<t>", spec);
    CHECK(rc == 0, "spec string parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return -1; }
    rc = buf_grammar_build(&g, &rx);
    CHECK(rc == 0, "grammar builds");
    if (rc != 0) { printf("  -> %s\n", g.error); return -1; }
    if (buf_nfa_build(&nfa, &rx) != 0) { printf("  -> %s\n", nfa.error); return -1; }
    if (buf_dfa_build(&dfa, &nfa, &rx) != 0) { printf("  -> %s\n", dfa.error); return -1; }
    {
        int p;
        for (p = 0; p < rx.prod_count; p++) {
            prod_lhs[p] = buf_lalr_plhs(&rx, p);
            prod_len[p] = buf_lalr_plen(&rx, p);
        }
    }
    return 0;
}

static int run_parse(BufParser *ps, const char *src) {
    BufLexer lx;
    buf_lexer_init(&lx, src, (int)strlen(src));
    buf_parser_init(ps, nodes, NODE_CAP, child, CHILD_CAP,
                    state_stack, node_stack, STACK_CAP);
    return buf_parse(ps, &lx,
                     dfa.cls, dfa.next, dfa.accept, dfa.rule_token,
                     dfa.nstates, dfa.nclass, dfa.start,
                     g.action, g.goto_tab, prod_lhs, prod_len,
                     g.ntok, g.nnonterm, g.start_state);
}

/* --- S-expression serializer, for pinned tree-shape checks --------------- */

static char sexpr_buf[4096];
static int  sexpr_len;

static void sexpr_append(const char *s, int n) {
    int i;
    for (i = 0; i < n && sexpr_len < (int)sizeof(sexpr_buf) - 1; i++)
        sexpr_buf[sexpr_len++] = s[i];
}

static void sexpr_node(int idx) {
    BufCstNode *n = &nodes[idx];
    if (n->is_terminal) {
        sexpr_append(n->token.lexeme, n->token.length);
        return;
    }
    sexpr_append("(", 1);
    sexpr_append(rx.nonterms[n->index].name, (int)strlen(rx.nonterms[n->index].name));
    {
        int i;
        for (i = 0; i < n->nchild; i++) {
            sexpr_append(" ", 1);
            sexpr_node(child[n->child_off + i]);
        }
    }
    sexpr_append(")", 1);
}

static const char *sexpr(int root) {
    sexpr_len = 0;
    sexpr_node(root);
    sexpr_buf[sexpr_len] = '\0';
    return sexpr_buf;
}

/* --- accept cases: tree shape proves precedence --------------------------- */

static void test_calc_accept_shape(void) {
    BufParser ps;
    int root;

    if (build("examples/calc.bflo") != 0) return;

    root = run_parse(&ps, "1+2*3");
    CHECK(root >= 0 && ps.status == BUF_PARSE_OK, "'1+2*3' parses");
    if (root >= 0)
        CHECK(strcmp(sexpr(root), "(expr (expr (term (factor 1))) + (term (term (factor 2)) * (factor 3)))") == 0,
              "'1+2*3' nests STAR under the right-hand term of PLUS (precedence)");

    root = run_parse(&ps, "(1+2)*x");
    CHECK(root >= 0 && ps.status == BUF_PARSE_OK, "'(1+2)*x' parses");
    if (root >= 0)
        CHECK(strcmp(sexpr(root),
                     "(expr (term (term (factor ( (expr (expr (term (factor 1))) + (term (factor 2))) ))) * (factor x)))") == 0,
              "'(1+2)*x' nests the parenthesised expr under the left-hand term of STAR");
}

/* --- reject cases: matches t_grammar.c's test_calc_reference_driver ------ */

static void test_calc_reject(void) {
    BufParser ps;

    if (build("examples/calc.bflo") != 0) return;

    CHECK(run_parse(&ps, "1+") < 0 && ps.status == BUF_PARSE_ERR_SYNTAX, "'1+' rejects");
    CHECK(run_parse(&ps, "(1") < 0 && ps.status == BUF_PARSE_ERR_SYNTAX, "'(1' rejects");
    CHECK(run_parse(&ps, "1 2") < 0 && ps.status == BUF_PARSE_ERR_SYNTAX, "'1 2' rejects");
}

/* --- the checked-in lex+parse example (examples/expr.bflo) --------------- */

static void test_expr_example(void) {
    BufParser ps;
    int root;

    if (build("examples/expr.bflo") != 0) return;

    /* examples/expr_main.c hardcodes nt index -> name (it has no BufRx at
     * runtime). Pin the reader's assignment so a production reorder that
     * shifts the indices fails here, not silently in the demo's output. */
    CHECK(rx.nonterm_count == 3
          && strcmp(rx.nonterms[0].name, "expr")   == 0
          && strcmp(rx.nonterms[1].name, "term")   == 0
          && strcmp(rx.nonterms[2].name, "factor") == 0,
          "expr.bflo nonterminals are indexed expr=0, term=1, factor=2");

    root = run_parse(&ps, "1+2*3");
    CHECK(root >= 0 && ps.status == BUF_PARSE_OK, "expr.bflo: '1+2*3' parses");
    if (root >= 0)
        CHECK(strcmp(sexpr(root),
                     "(expr (expr (term (factor 1))) + (term (term (factor 2)) * (factor 3)))") == 0,
              "expr.bflo: STAR binds tighter than PLUS");

    root = run_parse(&ps, "10 - 4 - 3");
    CHECK(root >= 0 && ps.status == BUF_PARSE_OK, "expr.bflo: '10 - 4 - 3' parses");
    if (root >= 0)
        CHECK(strcmp(sexpr(root),
                     "(expr (expr (expr (term (factor 10))) - (term (factor 4))) - (term (factor 3)))") == 0,
              "expr.bflo: MINUS is left-associative");

    CHECK(run_parse(&ps, "1 +") < 0 && ps.status == BUF_PARSE_ERR_SYNTAX,
          "expr.bflo: '1 +' rejects as ERR_SYNTAX");
    CHECK(run_parse(&ps, "2 @ 3") < 0 && ps.status == BUF_PARSE_ERR_SYNTAX
          && ps.error_tok.kind == BUF_TOK_ERROR,
          "expr.bflo: a stray '@' surfaces as a BUF_TOK_ERROR lookahead");
}

/* --- epsilon production: nchild == 0, inherited line:col ----------------- */

static void test_epsilon_production(void) {
    BufParser ps;
    int root;

    if (build_string(
            "%tokens INT\nINT [0-9]+\n%skip [ \\t\\r\\n]+\n"
            "%grammar\n%start args\nargs : | INT args ;\n") != 0)
        return;

    root = run_parse(&ps, "");
    CHECK(root >= 0 && ps.status == BUF_PARSE_OK, "empty input parses (epsilon base case)");
    if (root >= 0) {
        CHECK(nodes[root].nchild == 0, "epsilon reduce has zero children");
        CHECK(nodes[root].line == 1 && nodes[root].col == 1,
              "epsilon node inherits the current (EOF) lookahead's line:col");
    }

    root = run_parse(&ps, "1 2 3");
    CHECK(root >= 0 && ps.status == BUF_PARSE_OK, "'1 2 3' parses against the nullable list grammar");
}

/* --- pool exhaustion: reported as a status, not a crash ------------------ */

static void test_node_pool_exhaustion(void) {
    BufLexer lx;
    BufParser ps;
    BufCstNode tiny_nodes[2];
    int root;

    if (build("examples/calc.bflo") != 0) return;

    buf_lexer_init(&lx, "1+2*3", 5);
    buf_parser_init(&ps, tiny_nodes, 2, child, CHILD_CAP,
                    state_stack, node_stack, STACK_CAP);
    root = buf_parse(&ps, &lx,
                     dfa.cls, dfa.next, dfa.accept, dfa.rule_token,
                     dfa.nstates, dfa.nclass, dfa.start,
                     g.action, g.goto_tab, prod_lhs, prod_len,
                     g.ntok, g.nnonterm, g.start_state);
    CHECK(root < 0 && ps.status == BUF_PARSE_ERR_NODE_POOL,
          "an undersized node pool is reported as ERR_NODE_POOL, not a crash");
}

int main(void) {
    test_calc_accept_shape();
    test_calc_reject();
    test_expr_example();
    test_epsilon_production();
    test_node_pool_exhaustion();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "ok  ", checks, failures);
    return failures ? 1 : 0;
}
