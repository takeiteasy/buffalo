/*
 * json_main.c -- demo harness for the json lexer.
 *
 * Reads all of stdin, runs the generated json lexer, prints one line per
 * token: NAME "lexeme" line:col. Skips (whitespace) produce no line. Ends
 * at TOK_EOF. Same shape as calc_main.c / digits_main.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf_rt.h"
#include "json_tokens.h"

static const char *tok_name(int kind)
{
    switch (kind) {
    case TOK_EOF:      return "EOF";
    case TOK_ERROR:    return "ERROR";
    case TOK_LBRACE:   return "LBRACE";
    case TOK_RBRACE:   return "RBRACE";
    case TOK_LBRACKET: return "LBRACKET";
    case TOK_RBRACKET: return "RBRACKET";
    case TOK_COLON:    return "COLON";
    case TOK_COMMA:    return "COMMA";
    case TOK_STRING:   return "STRING";
    case TOK_NUMBER:   return "NUMBER";
    case TOK_TRUE:     return "TRUE";
    case TOK_FALSE:    return "FALSE";
    case TOK_NULL:     return "NULL";
    default:           return "?";
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
        printf("%-8s \"%.*s\" %d:%d\n",
               tok_name(t.kind), t.length, t.lexeme, t.line, t.col);
        if (t.kind == TOK_EOF)
            break;
    }
    return 0;
}
