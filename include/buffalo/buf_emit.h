/*
 * buf_emit.h -- buffalo emitter: a built BufDfa -> four file-scope tables
 * plus the `buf_next` wrapper, lowered into the generated .gen.c.
 *
 * UNLIKE buf_rx.h / buf_nfa.h / buf_dfa.h this header is NOT dual-compiled:
 * it uses cccc's reflection builtins (GlobalVar, MakeArray, Quote, ...) and
 * only ever reaches a compiler through `#include @comptime "buf_emit.h"` from
 * src/buf_comptime.c. The host unit tests never include it.
 *
 * Each table is one raw `GlobalVarSetInitData` blob -- a memcpy of the
 * table's live prefix, not thousands of literal nodes -- emitted `static
 * const`; the `buf_next` wrapper is a one-line `Quote()` that forwards to
 * buf_run. The generated `.gen.c` then builds with a plain `cc` against
 * runtime/buf_rt.c + an example `_main.c`; `make generated` / `make native`
 * exercise both lowering paths and diff their output.
 *
 * Table contract (see runtime/buf_rt.h's buf_run):
 *
 *   buf_dfa_class[256]              unsigned char, byte -> class 0..nclass-1
 *   buf_dfa_next[nstates*nclass]    short, flat row-major, -1 == dead
 *   buf_dfa_accept[nstates]         short, winning rule index or -1
 *   buf_rule_token[nrules]          short, rule index -> TOK_* value, -1 %skip
 *
 *   BufToken buf_next(BufLexer *lx) {
 *       return buf_run(lx, buf_dfa_class, buf_dfa_next, buf_dfa_accept,
 *                      buf_rule_token, nstates, nclass, start);
 *   }
 *
 * All four source arrays are already contiguous live prefixes of the BufDfa
 * arenas -- `next` is written at `si*nclass + k` with stride `dfa->nclass`
 * (the real class count, never BUF_DFA_MAX_CLASSES) -- so each blob is one
 * memcpy with no repacking. The blob length passed to GlobalVarSetInitData
 * MUST equal the emitted array type's ty->size, i.e. the live prefix, never
 * sizeof() the whole arena.
 *
 * Known limitation (carried in docs/design.md): the blob is a raw copy of the
 * comptime host's `short`s, so host width/endianness are baked in. Fine while
 * the comptime host and the target are the same machine.
 */
#ifndef BUF_EMIT_H
#define BUF_EMIT_H

#include "buf_dfa.h"
#ifdef BUF_EMIT_PARSER
#include "buf_grammar.h"
#endif

/* Emit the four tables + the buf_next wrapper for a built DFA. Returns 0.
 * `dfa` must have built cleanly (dfa->has_error == 0); the caller checks.
 *
 * Plain `static` in an @comptime-routed header -- the same shape buf_dfa.h's
 * helpers use, so buf_comptime.c's [[cccc::comptime]] entry can call it. */
static int buf_emit_tables(BufDfa *dfa, BufRx *rx) {
    /* The class table emits as `BufClass` (`typedef unsigned char` in
     * buf_rt.h), matching buf_run's `const unsigned char *cls` parameter and
     * the hand-written examples/digits_tables.c. The typedef is reachable as
     * an emitted global's element type because buf_rt.h arrives via `#include
     * @shared` and the generated forward-declaration block now sits below
     * that include. A bare GetType("unsigned char") still returns NULL (no
     * multi-word base-type spelling in the comptime type resolver), so the
     * table type must go through the typedef name. MakeConst() gives each
     * table the `static const` qualification digits_tables.c uses. */
    Type *class_ty = MakeConst(GetType("BufClass"));
    Type *short_ty = MakeConst(GetType("short"));
    int   ntrans   = dfa->nstates * dfa->nclass;
    int   sh       = (int)sizeof(short);
    Obj  *v_cls, *v_next, *v_accept, *v_rtok, *fn;
    Node *lx;

    /* rx is unused here: rule names, the %tokens header and TOK_* spellings
     * are all baked into dfa->rule_token numerically. buf_emit_parser_tables
     * below is the one that reads it (production shapes). */
    (void)rx;

    v_cls = GlobalVar("buf_dfa_class", MakeArray(class_ty, 256));
    GlobalVarSetInitData(v_cls, dfa->cls, 256);
    GlobalVarSetStatic(v_cls, 1);
    PublishNodeAt(v_cls, SyntheticToken("buf_dfa_class"));

    v_next = GlobalVar("buf_dfa_next", MakeArray(short_ty, ntrans));
    GlobalVarSetInitData(v_next, dfa->next, ntrans * sh);
    GlobalVarSetStatic(v_next, 1);
    PublishNodeAt(v_next, SyntheticToken("buf_dfa_next"));

    v_accept = GlobalVar("buf_dfa_accept", MakeArray(short_ty, dfa->nstates));
    GlobalVarSetInitData(v_accept, dfa->accept, dfa->nstates * sh);
    GlobalVarSetStatic(v_accept, 1);
    PublishNodeAt(v_accept, SyntheticToken("buf_dfa_accept"));

    v_rtok = GlobalVar("buf_rule_token", MakeArray(short_ty, dfa->nrules));
    GlobalVarSetInitData(v_rtok, dfa->rule_token, dfa->nrules * sh);
    GlobalVarSetStatic(v_rtok, 1);
    PublishNodeAt(v_rtok, SyntheticToken("buf_rule_token"));

    fn = MakeFunction("buf_next", GetType("BufToken"));
    FunctionAddParam(fn, "lx", MakePointer(GetType("BufLexer")));
    WithFn(fn) {
        lx = MakeParamRef(fn, "lx");
        FunctionSetBody(fn, Quote(
            "return buf_run($1, buf_dfa_class, "
            "buf_dfa_next, buf_dfa_accept, buf_rule_token, $2, $3, $4);",
            lx,
            MakeIntLiteral(dfa->nstates),
            MakeIntLiteral(dfa->nclass),
            MakeIntLiteral(dfa->start)));
    }
    return 0;
}

#ifdef BUF_EMIT_PARSER
/* prod_lhs[] / prod_len[] are not fields of BufGrammar -- buf_parse takes them
 * as flat `const int *` arrays baked by the caller from buf_lalr_plhs /
 * buf_lalr_plen (buf_grammar.h says these accessors exist for exactly this).
 * File-scope scratch so the bake-out is not a comptime-VM stack local; sized
 * to the real production arena, indexed only over 0..rx->prod_count-1 (the
 * synthetic S' -> %start augmenting production is never emitted). */
static int buf_emit_prod_lhs[BUF_RX_MAX_PRODS];
static int buf_emit_prod_len[BUF_RX_MAX_PRODS];

/* Emit the LALR(1) parser tables + the buf_parse_tree wrapper. Returns 0.
 * Runs AFTER buf_emit_tables -- the wrapper's Quote() template names the four
 * DFA table symbols that call publishes (buf_dfa_class ... buf_rule_token).
 * `dfa` supplies the DFA scalars the wrapper also forwards; `g` must have
 * built cleanly (g->has_error == 0; the caller checks).
 *
 * Same conventions as buf_emit_tables: raw GlobalVarSetInitData blobs of each
 * table's live prefix (g->action / g->goto_tab stride by g->ntok / g->nnonterm,
 * never the arena max, so each is one contiguous memcpy), `static const int`
 * element type, PublishNodeAt so a same-parse-point Quote() can name them. */
static int buf_emit_parser_tables(BufDfa *dfa, BufGrammar *g, BufRx *rx) {
    Type *int_ty  = MakeConst(GetType("int"));
    int   in      = (int)sizeof(int);
    int   naction = g->nstates * g->ntok;
    int   ngoto   = g->nstates * g->nnonterm;
    int   np      = rx->prod_count;
    Obj  *v_action, *v_goto, *v_plhs, *v_plen, *fn;
    Node *ps, *lx;
    int   p;

    for (p = 0; p < np; p++) {
        buf_emit_prod_lhs[p] = buf_lalr_plhs(rx, p);
        buf_emit_prod_len[p] = buf_lalr_plen(rx, p);
    }

    v_action = GlobalVar("buf_lalr_action", MakeArray(int_ty, naction));
    GlobalVarSetInitData(v_action, g->action, naction * in);
    GlobalVarSetStatic(v_action, 1);
    PublishNodeAt(v_action, SyntheticToken("buf_lalr_action"));

    v_goto = GlobalVar("buf_lalr_goto", MakeArray(int_ty, ngoto));
    GlobalVarSetInitData(v_goto, g->goto_tab, ngoto * in);
    GlobalVarSetStatic(v_goto, 1);
    PublishNodeAt(v_goto, SyntheticToken("buf_lalr_goto"));

    v_plhs = GlobalVar("buf_lalr_prod_lhs", MakeArray(int_ty, np));
    GlobalVarSetInitData(v_plhs, buf_emit_prod_lhs, np * in);
    GlobalVarSetStatic(v_plhs, 1);
    PublishNodeAt(v_plhs, SyntheticToken("buf_lalr_prod_lhs"));

    v_plen = GlobalVar("buf_lalr_prod_len", MakeArray(int_ty, np));
    GlobalVarSetInitData(v_plen, buf_emit_prod_len, np * in);
    GlobalVarSetStatic(v_plen, 1);
    PublishNodeAt(v_plen, SyntheticToken("buf_lalr_prod_len"));

    fn = MakeFunction("buf_parse_tree", GetType("int"));
    FunctionAddParam(fn, "ps", MakePointer(GetType("BufParser")));
    FunctionAddParam(fn, "lx", MakePointer(GetType("BufLexer")));
    WithFn(fn) {
        ps = MakeParamRef(fn, "ps");
        lx = MakeParamRef(fn, "lx");
        FunctionSetBody(fn, Quote(
            "return buf_parse($1, $2, buf_dfa_class, buf_dfa_next, "
            "buf_dfa_accept, buf_rule_token, $3, $4, $5, "
            "buf_lalr_action, buf_lalr_goto, buf_lalr_prod_lhs, "
            "buf_lalr_prod_len, $6, $7, $8);",
            ps, lx,
            MakeIntLiteral(dfa->nstates),
            MakeIntLiteral(dfa->nclass),
            MakeIntLiteral(dfa->start),
            MakeIntLiteral(g->ntok),
            MakeIntLiteral(g->nnonterm),
            MakeIntLiteral(g->start_state)));
    }
    return 0;
}
#endif /* BUF_EMIT_PARSER */

#endif /* BUF_EMIT_H */
