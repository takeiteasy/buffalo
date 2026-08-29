/*
 * digits_main.c -- M0 demo harness.
 *
 * Reads all of stdin, runs the hand-written digits lexer, prints one line
 * per token: NAME "lexeme" line:col. Skips (whitespace) produce no line.
 * Ends at TOK_EOF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf_rt.h"
#include "digits_tokens.h"

static const char *tok_name(int kind)
{
    switch (kind) {
    case TOK_EOF:   return "EOF";
    case TOK_ERROR: return "ERROR";
    case TOK_INT:   return "INT";
    default:        return "?";
    }
}

int main(void)
{
    static char buf[1 << 16];
    size_t len = fread(buf, 1, sizeof(buf), stdin);

    BufLexer lx;
    buf_lexer_init(&lx, buf, (int)len);

    for (;;) {
        BufToken t = buf_next(&lx);
        printf("%-5s \"%.*s\" %d:%d\n",
               tok_name(t.kind), t.length, t.lexeme, t.line, t.col);
        if (t.kind == TOK_EOF)
            break;
    }
    return 0;
}
