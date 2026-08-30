/*
 * calc_main.c -- demo harness for the calc lexer.
 *
 * Reads all of stdin, runs the generated calc lexer, prints one line per
 * token: NAME "lexeme" line:col. Skips (whitespace, `# ...` comments)
 * produce no line. Ends at TOK_EOF. Same shape as digits_main.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf_rt.h"
#include "calc_tokens.h"

static const char *tok_name(int kind)
{
    switch (kind) {
    case TOK_EOF:    return "EOF";
    case TOK_ERROR:  return "ERROR";
    case TOK_INT:    return "INT";
    case TOK_FLOAT:  return "FLOAT";
    case TOK_IDENT:  return "IDENT";
    case TOK_PLUS:   return "PLUS";
    case TOK_STAR:   return "STAR";
    case TOK_LPAREN: return "LPAREN";
    case TOK_RPAREN: return "RPAREN";
    default:         return "?";
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
        printf("%-6s \"%.*s\" %d:%d\n",
               tok_name(t.kind), t.length, t.lexeme, t.line, t.col);
        if (t.kind == TOK_EOF)
            break;
    }
    return 0;
}
