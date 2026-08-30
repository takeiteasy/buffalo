/*
 * t_rx.c -- host unit tests for include/buffalo/buf_rx.h.
 *
 * Plain `cc`, no cccc. Exercises the .bflo spec layer, the regex grammar, the
 * %grammar section (productions, %start, terminal/nonterminal resolution),
 * the per-node line:col tracking, and the empty-match (nullable) rejection.
 */
#include <stdio.h>
#include <string.h>

#include "buf_rx.h"

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

static BufRx rx; /* large; keep it out of the stack */

/* Parse a one-rule spec `NAME <regex>` (NAME listed in %tokens) and return
 * the root node of rule 0, or NULL on error. */
static BufRxNode *parse_one(const char *regex)
{
    char spec[512];
    snprintf(spec, sizeof(spec), "%%tokens A\nA %s\n", regex);
    if (buf_rx_parse_string(&rx, "<t>", spec) != 0)
        return NULL;
    return &rx.nodes[rx.rules[0].root];
}

static int only_bytes(const unsigned char *bits, const char *set)
{
    int c;
    for (c = 0; c < 256; c++) {
        int want = 0;
        const char *p;
        for (p = set; *p; p++)
            if ((unsigned char)*p == c) want = 1;
        if (buf_rx_bits_get(bits, c) != want)
            return 0;
    }
    return 1;
}

static int bits_has(const unsigned char *bits, int c) {
    return buf_rx_bits_get(bits, c);
}

static void test_calc_spec(void)
{
    static const char *want[] = { "INT", "FLOAT", "IDENT", "PLUS",
                                  "STAR", "LPAREN", "RPAREN" };
    int i, rc;

    rc = buf_rx_read_file(&rx, "examples/calc.bflo");
    CHECK(rc == 0, "examples/calc.bflo parses cleanly");
    if (rc != 0) { printf("  -> %s\n", rx.error); return; }

    CHECK(rx.token_count == 7, "calc.bflo has 7 %tokens");
    for (i = 0; i < 7 && i < rx.token_count; i++)
        CHECK(strcmp(rx.tokens[i], want[i]) == 0, "calc.bflo %tokens order");

    CHECK(rx.rule_count == 9, "calc.bflo has 9 rules (7 named + 2 %skip)");
    CHECK(strcmp(rx.rules[0].name, "INT") == 0, "rule 0 is INT");
    CHECK(rx.rules[0].is_skip == 0, "rule 0 is not %skip");
    CHECK(rx.rules[0].tok_index == 0, "rule 0 maps to %tokens slot 0");
    CHECK(rx.rules[7].is_skip == 1, "rule 7 is %skip");
    CHECK(rx.rules[8].is_skip == 1, "rule 8 is %skip");
    CHECK(rx.rules[8].tok_index == -1, "%skip rule has no token slot");
}

static void test_grammar_shapes(void)
{
    BufRxNode *n;

    n = parse_one("[0-9]+");
    CHECK(n && n->kind == BUF_RX_PLUS, "[0-9]+ -> PLUS");
    if (n) {
        BufRxNode *c = &rx.nodes[n->a];
        CHECK(c->kind == BUF_RX_CLASS, "PLUS child is CLASS");
        CHECK(only_bytes(c->bits, "0123456789"), "[0-9] is exactly 0..9");
    }

    n = parse_one("[a-z]");
    CHECK(n && n->kind == BUF_RX_CLASS, "[a-z] -> CLASS");
    CHECK(n && bits_has(n->bits, 'a') && bits_has(n->bits, 'z') &&
              !bits_has(n->bits, 'A'), "[a-z] range");

    n = parse_one("[^0-9]");
    CHECK(n && n->kind == BUF_RX_CLASS, "[^0-9] -> CLASS");
    CHECK(n && !bits_has(n->bits, '5') && bits_has(n->bits, 'x') &&
              bits_has(n->bits, '\n'), "[^0-9] negation includes newline");

    n = parse_one("\\d");
    CHECK(n && n->kind == BUF_RX_CLASS && only_bytes(n->bits, "0123456789"),
          "\\d desugars to [0-9]");

    n = parse_one("\\w");
    CHECK(n && bits_has(n->bits, '_') && bits_has(n->bits, 'A') &&
              bits_has(n->bits, '9') && !bits_has(n->bits, '-'),
          "\\w is [A-Za-z0-9_]");

    n = parse_one("\"ab\"");
    CHECK(n && n->kind == BUF_RX_CONCAT, "\"ab\" -> CONCAT");
    if (n && n->kind == BUF_RX_CONCAT) {
        CHECK(rx.nodes[n->a].kind == BUF_RX_CLASS &&
                  bits_has(rx.nodes[n->a].bits, 'a'),
              "\"ab\" left leaf is 'a'");
        CHECK(rx.nodes[n->b].kind == BUF_RX_CLASS &&
                  bits_has(rx.nodes[n->b].bits, 'b'),
              "\"ab\" right leaf is 'b'");
    }

    n = parse_one("a|b");
    CHECK(n && n->kind == BUF_RX_ALT, "a|b -> ALT");

    n = parse_one("ab");
    CHECK(n && n->kind == BUF_RX_CONCAT, "ab -> CONCAT");

    n = parse_one(".");
    CHECK(n && n->kind == BUF_RX_CLASS && !bits_has(n->bits, '\n') &&
              bits_has(n->bits, 'a') && bits_has(n->bits, ' '),
          ". is any byte but newline");

    n = parse_one("(a?)b");
    CHECK(n && n->kind == BUF_RX_CONCAT, "(a?)b -> CONCAT");
    CHECK(n && rx.nodes[n->a].kind == BUF_RX_OPT, "(a?) -> OPT");

    n = parse_one("(ab|c)*d");
    CHECK(n && n->kind == BUF_RX_CONCAT, "(ab|c)*d top is CONCAT");
    CHECK(n && rx.nodes[n->a].kind == BUF_RX_STAR, "(ab|c)* -> STAR");
    CHECK(n && rx.nodes[rx.nodes[n->a].a].kind == BUF_RX_ALT,
          "STAR child is ALT");

    /* the metacharacter escapes */
    n = parse_one("\\+");
    CHECK(n && n->kind == BUF_RX_CLASS && only_bytes(n->bits, "+"),
          "\\+ is a literal plus");
    n = parse_one("\\n");
    CHECK(n && n->kind == BUF_RX_CLASS && bits_has(n->bits, '\n'),
          "\\n is a literal newline");

    /* whitespace between atoms is insignificant */
    n = parse_one("\"#\" [^\\n]*");
    CHECK(n && n->kind == BUF_RX_CONCAT,
          "space between atoms concatenates (comment pattern)");
}

/* Reference calc-shaped spec: %tokens/rules identical to examples/calc.bflo,
 * plus a %grammar section for expr/term/factor. */
static const char *calc_grammar_spec =
    "%tokens INT PLUS STAR LPAREN RPAREN\n"
    "\n"
    "INT       [0-9]+\n"
    "PLUS      \"+\"\n"
    "STAR      \"*\"\n"
    "LPAREN    \"(\"\n"
    "RPAREN    \")\"\n"
    "\n"
    "%skip     [ \\t\\r\\n]+\n"
    "\n"
    "%grammar\n"
    "%start expr\n"
    "\n"
    "expr : expr PLUS term\n"
    "     | term\n"
    "     ;\n"
    "\n"
    "term : term STAR factor\n"
    "     | factor\n"
    "     ;\n"
    "\n"
    "factor : LPAREN expr RPAREN\n"
    "       | INT\n"
    "       ;\n";

static void test_grammar_section(void)
{
    int rc;

    /* regression: no %grammar section still parses exactly as before */
    rc = buf_rx_parse_string(&rx, "<t>", "%tokens A\nA a\n");
    CHECK(rc == 0, "spec without %grammar still parses");
    CHECK(rx.has_grammar == 0, "no %grammar -> has_grammar is 0");
    CHECK(rx.start_nt == -1, "no %grammar -> start_nt is -1");

    rc = buf_rx_parse_string(&rx, "<t>", calc_grammar_spec);
    CHECK(rc == 0, "calc-shaped grammar spec parses cleanly");
    if (rc != 0) { printf("  -> %s\n", rx.error); return; }

    CHECK(rx.has_grammar == 1, "has_grammar is set");
    CHECK(rx.nonterm_count == 3, "expr/term/factor -> 3 nonterminals");
    CHECK(rx.start_nt >= 0 &&
              strcmp(rx.nonterms[rx.start_nt].name, "expr") == 0,
          "%start resolves to 'expr'");
    CHECK(rx.prod_count == 6, "6 alternatives across 3 productions");

    /* prod 0: expr : expr PLUS term  (left-recursive, multi-symbol rhs) */
    CHECK(strcmp(rx.nonterms[rx.prods[0].lhs].name, "expr") == 0,
          "prod 0 lhs is expr");
    CHECK(rx.prods[0].rhs_len == 3, "prod 0 has 3 rhs symbols");
    {
        BufSym *s0 = &rx.syms[rx.prods[0].rhs_start + 0];
        BufSym *s1 = &rx.syms[rx.prods[0].rhs_start + 1];
        BufSym *s2 = &rx.syms[rx.prods[0].rhs_start + 2];
        CHECK(!s0->is_terminal &&
                  strcmp(rx.nonterms[s0->index].name, "expr") == 0,
              "prod 0 rhs[0] is nonterminal expr");
        CHECK(s1->is_terminal && strcmp(rx.tokens[s1->index], "PLUS") == 0,
              "prod 0 rhs[1] is terminal PLUS");
        CHECK(!s2->is_terminal &&
                  strcmp(rx.nonterms[s2->index].name, "term") == 0,
              "prod 0 rhs[2] is nonterminal term");
    }

    /* prod 5: factor : INT (single-symbol alternative) */
    CHECK(rx.prods[5].rhs_len == 1, "prod 5 has 1 rhs symbol");
    CHECK(rx.syms[rx.prods[5].rhs_start].is_terminal &&
              strcmp(rx.tokens[rx.syms[rx.prods[5].rhs_start].index], "INT") == 0,
          "prod 5 rhs[0] is terminal INT");

    /* epsilon alternative */
    rc = buf_rx_parse_string(&rx, "<t>",
                             "%tokens INT\nINT [0-9]+\n%grammar\n"
                             "%start expr\nexpr : INT | ;\n");
    CHECK(rc == 0, "epsilon alternative parses");
    CHECK(rc == 0 && rx.prod_count == 2, "epsilon alt still 2 productions");
    CHECK(rc == 0 && rx.prods[1].rhs_len == 0, "epsilon alt has rhs_len 0");

    /* classification is exact-match, not case-folded */
    rc = buf_rx_parse_string(&rx, "<t>",
                             "%tokens EXPR\nEXPR [0-9]+\n%grammar\n"
                             "%start expr\nexpr : EXPR ;\n");
    CHECK(rc == 0, "EXPR (token) vs expr (nonterminal) both parse");
    if (rc == 0) {
        BufSym *s = &rx.syms[rx.prods[0].rhs_start];
        CHECK(s->is_terminal && strcmp(rx.tokens[s->index], "EXPR") == 0,
              "'EXPR' rhs symbol classifies as the token, not case-folded");
        CHECK(strcmp(rx.nonterms[rx.prods[0].lhs].name, "expr") == 0,
              "'expr' lhs stays a distinct nonterminal from 'EXPR'");
    }
}

/* Every entry: a spec that must fail, and a substring its error must carry. */
static void test_errors(void)
{
    struct { const char *spec; const char *needle; const char *what; } cases[] = {
        { "%tokens A\nA [a-\n",        ":2:3:",
          "unterminated class reports the '[' position" },
        { "%tokens A\nA [a-\n",        "unterminated character class",
          "unterminated class message" },
        { "%tokens A\nA (ab\n",        "unterminated '(' group",
          "unterminated group" },
        { "%tokens A\nA \"ab\n",       "unterminated string literal",
          "unterminated string" },
        { "%tokens A\nA a{2,3}\n",     "{n,m}",
          "{n,m} rejected" },
        { "%foo bar\n",                "unknown directive '%foo'",
          "unknown directive" },
        { "%tokens B\nA [0-9]+\n",     "rule 'A' is not listed in %tokens",
          "rule missing from %tokens" },
        { "%tokens A B\nA a\n",        "%tokens entry 'B' has no matching rule",
          "%tokens entry with no rule" },
        { "%tokens EOF\n",             "'EOF' is reserved",
          "EOF reserved in %tokens" },
        { "%tokens ERROR\n",           "'ERROR' is reserved",
          "ERROR reserved in %tokens" },
        { "%tokens A\nA [ ]*\n",       "rule 'A' can match the empty string",
          "nullable named rule rejected" },
        { "%tokens A\nA a\n%skip [ \\t]*\n",
          "%skip rule can match the empty string",
          "nullable %skip rule rejected" },
        { "%tokens A\nA (a|b?)\n",     "can match the empty string",
          "nullable via alternation rejected" },
        { "# just a comment\n",        "%tokens directive is required",
          "%tokens is required" },
        { "%tokens A\nA [z-a]\n",      "reversed range",
          "reversed range in class" },
        { "%tokens A\nA \\q\n",        "unknown escape",
          "unknown escape" },
        { "%tokens A\nA *\n",          "nothing to repeat",
          "nothing to repeat" },
        { "%tokens A\nA\n",            "rule 'A' has no regex",
          "rule with no regex" },
        { "%tokens A\n1BAD x\n",       "unexpected character",
          "rule name must start with a letter or _" },

        /* %grammar section */
        { "%tokens A\nA a\n%grammar\nexpr : A ;\n",
          "the %start directive is required",
          "%grammar without %start" },
        { "%tokens A\nA a\n%grammar\n%start expr\nterm : A ;\n",
          "%start symbol 'expr' has no production",
          "%start names an undefined nonterminal" },
        { "%tokens A\nA a\n%grammar\n%start expr\nexpr : A | facter ;\n",
          "nonterminal 'facter' is used but never defined",
          "undefined nonterminal used in a production" },
        { "%tokens A\nA a\n%grammar\n%start expr\nA : A ;\n",
          "'A' is a %tokens terminal and cannot be a production left-hand side",
          "a token cannot be a production lhs" },
        { "%tokens A\nA a\n%grammar\n%start A\nexpr : A ;\n",
          "'A' is a %tokens terminal and cannot be a %start symbol",
          "a token cannot be the %start symbol" },
        { "%tokens A\nA a\n%start expr\n%grammar\nexpr : A ;\n",
          "%start is only valid inside a %grammar section",
          "%start before %grammar" },
        { "%tokens A\nA a\n%grammar\n%start expr\nexpr A ;\n",
          "expected ':' after nonterminal 'expr'",
          "missing ':' in a production" },
        { "%tokens A\nA a\n%grammar\n%start expr\nexpr : A\n",
          "expected ';' at end of production 'expr'",
          "missing ';' at end of a production" },
        { "%tokens A\nA a\n%grammar\n%start expr\nexpr : A @ ;\n",
          "unexpected character '@' in grammar",
          "stray character in a production rhs" },
        { "%tokens A\nA a\n%grammar\n%start expr\n1bad : A ;\n",
          "unexpected character '1' in grammar",
          "nonterminal name must start with a letter or _" },
    };
    int i;
    int nc = (int)(sizeof(cases) / sizeof(cases[0]));

    for (i = 0; i < nc; i++) {
        int rc = buf_rx_parse_string(&rx, "<t>", cases[i].spec);
        CHECK(rc != 0, cases[i].what);
        if (rc == 0) { printf("  (did not fail: %s)\n", cases[i].what); continue; }
        CHECK(strstr(rx.error, cases[i].needle) != NULL, cases[i].what);
        if (!strstr(rx.error, cases[i].needle))
            printf("  error was: %s  (wanted substring: %s)\n",
                   rx.error, cases[i].needle);
    }
}

static void test_linecol(void)
{
    /* rule name column, and a mid-line regex error column */
    CHECK(buf_rx_parse_string(&rx, "<t>",
                              "%tokens A\n\nA   x[0-9\n") != 0,
          "unterminated class on line 3");
    CHECK(strstr(rx.error, "<t>:3:6:") != NULL,
          "error points at the '[' on line 3, col 6");
    if (!strstr(rx.error, "<t>:3:6:"))
        printf("  error was: %s\n", rx.error);

    /* a mid-grammar error, several lines into a %grammar section, carries
     * the exact line:col of the offending symbol */
    CHECK(buf_rx_parse_string(&rx, "<t>",
                              "%tokens A\nA a\n\n%grammar\n%start expr\n\n"
                              "expr : A\n     | facter\n     ;\n") != 0,
          "undefined nonterminal several lines into %grammar");
    CHECK(strstr(rx.error, "<t>:8:8:") != NULL,
          "error points at 'facter' on line 8, col 8");
    if (!strstr(rx.error, "<t>:8:8:"))
        printf("  error was: %s\n", rx.error);
}

int main(void)
{
    test_calc_spec();
    test_grammar_shapes();
    test_grammar_section();
    test_errors();
    test_linecol();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "ok  ", checks, failures);
    return failures ? 1 : 0;
}
