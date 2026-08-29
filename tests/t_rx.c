/*
 * t_rx.c -- host unit tests for include/buffalo/buf_rx.h.
 *
 * Plain `cc`, no cccc. Exercises the .l spec layer, the regex grammar, the
 * per-node line:col tracking, and the empty-match (nullable) rejection.
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

    rc = buf_rx_read_file(&rx, "examples/calc.l");
    CHECK(rc == 0, "examples/calc.l parses cleanly");
    if (rc != 0) { printf("  -> %s\n", rx.error); return; }

    CHECK(rx.token_count == 7, "calc.l has 7 %tokens");
    for (i = 0; i < 7 && i < rx.token_count; i++)
        CHECK(strcmp(rx.tokens[i], want[i]) == 0, "calc.l %tokens order");

    CHECK(rx.rule_count == 9, "calc.l has 9 rules (7 named + 2 %skip)");
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
}

int main(void)
{
    test_calc_spec();
    test_grammar_shapes();
    test_errors();
    test_linecol();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "ok  ", checks, failures);
    return failures ? 1 : 0;
}
