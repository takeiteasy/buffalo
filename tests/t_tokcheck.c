/*
 * t_tokcheck.c -- host unit tests for include/buffalo/buf_tokcheck.h.
 *
 * Plain `cc`, no cccc. Checks that a token header agreeing with a spec's
 * %tokens list passes, and that every kind of drift is reported.
 */
#include <stdio.h>
#include <string.h>

#include "buf_rx.h"
#include "buf_tokcheck.h"

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

static BufRx rx;
static BufTc tc;

/* calc.bflo's %tokens list, in a spec small enough to inline. */
static const char *CALC_SPEC =
    "%tokens INT FLOAT IDENT PLUS STAR LPAREN RPAREN\n"
    "INT    [0-9]+\n"
    "FLOAT  [0-9]+\\.[0-9]+\n"
    "IDENT  [a-zA-Z_][a-zA-Z0-9_]*\n"
    "PLUS   \"+\"\n"
    "STAR   \"*\"\n"
    "LPAREN \"(\"\n"
    "RPAREN \")\"\n";

static int check_header(const char *hdr, const char *needle, const char *what)
{
    int rc;
    CHECK(buf_rx_parse_string(&rx, "<spec>", CALC_SPEC) == 0, "spec parses");
    buf_tc_load_string(&tc, "<hdr>", hdr);
    rc = buf_tc_check(&tc, &rx);
    if (needle == NULL) {
        CHECK(rc == 0, what);
        if (rc != 0) printf("  unexpected error: %s\n", tc.error);
        return rc == 0;
    }
    CHECK(rc != 0, what);
    if (rc == 0) { printf("  (did not fail: %s)\n", what); return 0; }
    CHECK(strstr(tc.error, needle) != NULL, what);
    if (!strstr(tc.error, needle))
        printf("  error was: %s  (wanted: %s)\n", tc.error, needle);
    return 1;
}

int main(void)
{
    /* the real checked-in header against the real spec */
    {
        int rc;
        CHECK(buf_rx_read_file(&rx, "examples/calc.bflo") == 0, "calc.bflo parses");
        CHECK(buf_tc_read_file(&tc, "examples/calc_tokens.h") == 0,
              "calc_tokens.h reads");
        rc = buf_tc_check(&tc, &rx);
        CHECK(rc == 0, "examples/calc_tokens.h agrees with examples/calc.bflo");
        if (rc != 0) printf("  -> %s\n", tc.error);
    }

    /* a well-formed inline header passes */
    check_header(
        "#ifndef CALC_TOKENS_H\n#define CALC_TOKENS_H\n"
        "enum {\n"
        "  TOK_EOF = 0, TOK_ERROR = 1,\n"
        "  TOK_INT, TOK_FLOAT, TOK_IDENT, TOK_PLUS, TOK_STAR,\n"
        "  TOK_LPAREN, TOK_RPAREN\n"
        "};\n#endif\n",
        NULL, "clean inline header agrees");

    /* comment / #define noise between guard and enum is tolerated */
    check_header(
        "#ifndef H\n#define H\n"
        "/* a block comment\n   spanning lines */\n"
        "// a line comment\n"
        "#define UNRELATED 3\n"
        "enum { TOK_EOF=0, TOK_ERROR=1, TOK_INT, TOK_FLOAT, TOK_IDENT,\n"
        "       TOK_PLUS, TOK_STAR, TOK_LPAREN, TOK_RPAREN };\n#endif\n",
        NULL, "comment and #define noise tolerated");

    /* reordered */
    check_header(
        "enum { TOK_EOF=0, TOK_ERROR=1, TOK_FLOAT, TOK_INT, TOK_IDENT,\n"
        "       TOK_PLUS, TOK_STAR, TOK_LPAREN, TOK_RPAREN };\n",
        "where 'TOK_INT' is expected", "reordered kinds reported");

    /* missing */
    check_header(
        "enum { TOK_EOF=0, TOK_ERROR=1, TOK_INT, TOK_FLOAT, TOK_IDENT,\n"
        "       TOK_PLUS, TOK_STAR, TOK_LPAREN };\n",
        "missing 'TOK_RPAREN'", "missing kind reported");

    /* extra */
    check_header(
        "enum { TOK_EOF=0, TOK_ERROR=1, TOK_INT, TOK_FLOAT, TOK_IDENT,\n"
        "       TOK_PLUS, TOK_STAR, TOK_LPAREN, TOK_RPAREN, TOK_EXTRA };\n",
        "extra entry 'TOK_EXTRA'", "extra kind reported");

    /* wrong reserved value */
    check_header(
        "enum { TOK_EOF=7, TOK_ERROR=1, TOK_INT, TOK_FLOAT, TOK_IDENT,\n"
        "       TOK_PLUS, TOK_STAR, TOK_LPAREN, TOK_RPAREN };\n",
        "TOK_EOF = 0", "wrong TOK_EOF value reported");

    check_header(
        "enum { TOK_EOF=0, TOK_ERROR=2, TOK_INT, TOK_FLOAT, TOK_IDENT,\n"
        "       TOK_PLUS, TOK_STAR, TOK_LPAREN, TOK_RPAREN };\n",
        "TOK_ERROR = 1", "wrong TOK_ERROR value reported");

    /* an explicit value pinned on a normal kind */
    check_header(
        "enum { TOK_EOF=0, TOK_ERROR=1, TOK_INT=2, TOK_FLOAT, TOK_IDENT,\n"
        "       TOK_PLUS, TOK_STAR, TOK_LPAREN, TOK_RPAREN };\n",
        "pins 'TOK_INT'", "explicit value on a normal kind reported");

    /* missing reserved slots entirely */
    check_header(
        "enum { TOK_INT, TOK_FLOAT, TOK_IDENT, TOK_PLUS, TOK_STAR,\n"
        "       TOK_LPAREN, TOK_RPAREN };\n",
        "where 'TOK_EOF' is expected", "missing reserved slots reported");

    /* no enum at all */
    check_header("#ifndef H\n#define H\n#endif\n",
                 "no `enum", "header without an enum reported");

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "ok  ", checks, failures);
    return failures ? 1 : 0;
}
