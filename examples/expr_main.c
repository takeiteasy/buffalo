/*
 * expr_main.c -- demo harness for the generated expr lexer + parser.
 *
 * Reads all of stdin and parses it one line at a time (%skip swallows
 * newlines, so a whole-buffer parse would run the lines together and a
 * syntax error on line 2 would mask a good parse on line 1). Blank lines
 * and lines whose first non-blank character is '#' are ignored (expr.bflo
 * has no comment rule of its own). For each remaining line it prints:
 *
 *   <line>  =>  <s-expr>                     on a successful parse
 *   <line>  =>  error: <what> at "<lexeme>" L:C  [state S]   otherwise
 *
 * The s-expression is the concrete syntax tree buf_parse_tree builds:
 * interior nodes as `(name ...)`, leaves as their lexeme. Same tree shape
 * the host test tests/t_parse.c pins.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf_rt.h"
#include "expr_tokens.h"

static const char *tok_name(int kind)
{
    switch (kind) {
    case TOK_EOF:    return "EOF";
    case TOK_ERROR:  return "ERROR";
    case TOK_INT:    return "INT";
    case TOK_PLUS:   return "PLUS";
    case TOK_MINUS:  return "MINUS";
    case TOK_STAR:   return "STAR";
    case TOK_SLASH:  return "SLASH";
    case TOK_LPAREN: return "LPAREN";
    case TOK_RPAREN: return "RPAREN";
    default:         return "?";
    }
}

/*
 * Nonterminal index -> name. buf_parse cannot take a BufRx*, so the CST's
 * interior `index` fields are bare nonterminal indices -- the spec reader
 * numbers a nonterminal by its first mention: `%start expr` is mention one
 * (index 0), then `term` and `factor` first appear in the expr/term RHSs,
 * in that order. tests/t_parse.c static-checks this mapping so a production
 * reorder that shifts it fails there rather than here.
 */
static const char *nt_name(int index)
{
    switch (index) {
    case 0:  return "expr";
    case 1:  return "term";
    case 2:  return "factor";
    default: return "?";
    }
}

#define NODE_CAP  512
#define CHILD_CAP 1024
#define STACK_CAP 256

static BufCstNode nodes[NODE_CAP];
static int        child[CHILD_CAP];
static int        state_stack[STACK_CAP];
static int        node_stack[STACK_CAP];

static void print_sexpr(int idx)
{
    BufCstNode *n = &nodes[idx];
    int i;

    if (n->is_terminal) {
        printf("%.*s", n->token.length, n->token.lexeme);
        return;
    }
    printf("(%s", nt_name(n->index));
    for (i = 0; i < n->nchild; i++) {
        printf(" ");
        print_sexpr(child[n->child_off + i]);
    }
    printf(")");
}

static const char *parse_err(int status)
{
    switch (status) {
    case BUF_PARSE_ERR_SYNTAX:     return "syntax";
    case BUF_PARSE_ERR_NODE_POOL:  return "node pool exhausted";
    case BUF_PARSE_ERR_CHILD_POOL: return "child pool exhausted";
    case BUF_PARSE_ERR_STACK:      return "parse stack exhausted";
    default:                       return "unknown";
    }
}

static void parse_line(const char *s, int len)
{
    BufLexer  lx;
    BufParser ps;
    int       root;

    printf("%.*s  =>  ", len, s);

    buf_lexer_init(&lx, s, len);
    buf_parser_init(&ps, nodes, NODE_CAP, child, CHILD_CAP,
                    state_stack, node_stack, STACK_CAP);
    root = buf_parse_tree(&ps, &lx);

    if (root >= 0 && ps.status == BUF_PARSE_OK) {
        print_sexpr(root);
        printf("\n");
        return;
    }

    printf("error: %s", parse_err(ps.status));
    if (ps.status == BUF_PARSE_ERR_SYNTAX) {
        printf(" at %s \"%.*s\" %d:%d [state %d]",
               tok_name(ps.error_tok.kind),
               ps.error_tok.length, ps.error_tok.lexeme,
               ps.error_tok.line, ps.error_tok.col,
               ps.error_state);
    }
    printf("\n");
}

int main(void)
{
    static char buf[1 << 16];
    size_t len = fread(buf, 1, sizeof(buf), stdin);
    size_t i = 0, start = 0;

    for (i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            int n = (int)(i - start);
            /* skip blank / whitespace-only lines and '# ...' comment lines */
            int j, skip = 1;
            for (j = 0; j < n; j++) {
                char c = buf[start + j];
                if (c == ' ' || c == '\t' || c == '\r')
                    continue;
                skip = (c == '#');
                break;
            }
            if (n > 0 && !skip)
                parse_line(buf + start, n);
            start = i + 1;
        }
    }
    return 0;
}
