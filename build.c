// Build script for buffalo. Run from the repository root with:
//
//     cccc --build build.c                                  # everything
//     cccc --build build.c --build-target=check             # the whole suite
//     cccc --build build.c --build-target=run-t_dfa         # one host test
//     cccc --build build.c --build-target=calc_native       # one native build
//     cccc --build build.c --build-option=bench=1           # per-phase costs
//     cccc --build build.c --build-cache                    # incremental
//
// Every example builds two ways, and both must produce identical output:
//
//   - generated — `bin/buffalo lex|parse` (cccc -c=generated over
//     src/buf_comptime.c with the spec as -D BUF_SPEC) writes an inspectable
//     build/NAME.bflo[.parse].gen.c, then the system cc links it against the
//     example's _main.c driver and the runtime. The portability proof:
//     nothing but a C compiler past that point.
//   - native — one whole-program `cccc --compile=native` invocation (a
//     CcccExecutable target): the comptime pass lowers the spec in the build
//     runner itself. This is the path the plain Makefile could not express --
//     a build target could not use cccc as its compiler.
//
// The comptime front half is ordinary .h/.c module pairs (src/buf_*.c
// declaring alongside include/buffalo/buf_*.h) that buf_comptime.c pulls
// straight in with `#include @comptime`, so they ride the include path. The
// same .c files also build with a plain cc, linked into the host unit tests;
// buf_grammar additionally rides the comptime pipeline under -D
// BUF_EMIT_PARSER (parser mode). buf_emit is comptime-VM only -- it uses
// cccc's reflection builtins, so no host test links it.
//
// digits is the one example with a hand-written reference table file
// (examples/digits_tables.c), so its check is a three-way parity:
// hand-written == generated == native. big.bflo stays out of the suite --
// bench/stress fixture, not a worked example (see the bench target).
//
// Host tests self-report pass/fail and exit nonzero on failure; the golden
// checks pipe each binary over examples/NAME.txt and diff against
// examples/NAME.expected. Executable outputs are kept flat in build/ at the
// stems the docs reference (build/digits, build/calc_gen, build/t_dfa, ...).

#include <stdio.h>
#include <string.h>

static const char *LEX_EXAMPLES[]   = {"digits", "calc", "clike", "json"};
static const char *PARSE_EXAMPLES[] = {"expr"};

// Pure-C comptime modules: `.h` declares, `src/buf_*.c` defines. `cc` links
// them into the host tests; src/buf_comptime.c `#include @comptime`s
// buf_rx/tokcheck/nfa/dfa always and buf_grammar under -D BUF_EMIT_PARSER.
// They are build inputs to the cccc targets but never command-line arguments.
static const char *CT_SRC[] = {"src/buf_rx.c", "src/buf_tokcheck.c",
                               "src/buf_nfa.c", "src/buf_dfa.c",
                               "src/buf_grammar.c"};
static const char *CT_HDRS[] = {
    "include/buffalo/buf_rx.h",      "include/buffalo/buf_tokcheck.h",
    "include/buffalo/buf_nfa.h",     "include/buffalo/buf_dfa.h",
    "include/buffalo/buf_grammar.h", "include/buffalo/buf_emit.h"};

// Host unit tests for the comptime headers -- plain cc, no cccc. The last
// three additionally link the runtime and drive the real buf_run over
// freshly built tables. t_tokgen also links src/buf_tokgen.c (a host-only
// module, not part of the comptime set) via the special case below.
static const char *PURE_TESTS[] = {"t_rx", "t_tokcheck", "t_nfa", "t_tokgen"};
static const char *RT_TESTS[]   = {"t_dfa", "t_grammar", "t_parse"};

// Inputs a `bin/buffalo` lowering reads besides its own sources -- the same
// prerequisite set the old Makefile's .gen.c rules listed. A RunCustom
// command gets no depfile, so its declared inputs drive the up-to-date skip
// check (under --build-cache).
static void add_spec_inputs(BuildTarget *t, const char *spec) {
    AddInput(t, spec);
    AddInput(t, "src/buf_comptime.c");
    for (int i = 0; i < (int)(sizeof(CT_SRC) / sizeof(*CT_SRC)); i++)
        AddInput(t, CT_SRC[i]);
    for (int i = 0; i < (int)(sizeof(CT_HDRS) / sizeof(*CT_HDRS)); i++)
        AddInput(t, CT_HDRS[i]);
    AddInput(t, "runtime/buf_rt.h");
}

[[cccc::build]]
int build_main(Builder *ctx) {
    BuildTarget *check =
        RunCustom(ctx, "check", "echo 'buffalo: all checks ok'");

    // -- host unit tests ---------------------------------------------------
    // Each test links every CT_SRC module (unused ones are dead code in the
    // link, not an error -- simpler than tracking a per-test subset).
    int n_pure = (int)(sizeof(PURE_TESTS) / sizeof(*PURE_TESTS));
    int n_rt   = (int)(sizeof(RT_TESTS) / sizeof(*RT_TESTS));
    for (int i = 0; i < n_pure + n_rt; i++) {
        const char *name = i < n_pure ? PURE_TESTS[i] : RT_TESTS[i - n_pure];
        int         with_rt = i >= n_pure;

        char path[128];
        snprintf(path, sizeof(path), "tests/%s.c", name);

        BuildTarget *t = Executable(ctx, name);
        SetOutput(t, name);
        AddSource(t, path);
        for (int s = 0; s < (int)(sizeof(CT_SRC) / sizeof(*CT_SRC)); s++)
            AddSource(t, CT_SRC[s]);
        if (strcmp(name, "t_tokgen") == 0)
            AddSource(t, "src/buf_tokgen.c");
        if (with_rt)
            AddSource(t, "runtime/buf_rt.c");
        AddInclude(t, "include/buffalo");
        AddInclude(t, "runtime");
        AddCFlag(t, "-O2");
        AddCFlag(t, "-Wall");

        char runname[64];
        snprintf(runname, sizeof(runname), "run-%s", name);
        BuildTarget *r = RunCustom(ctx, runname, TargetOutput(t));
        DependsOn(r, t);
        DependsOn(check, r);
    }

    // -- buf_tokgen: the opt-in token-header generator behind `buffalo
    // tokens`. Host-only, plain cc: it links buf_rx for the spec reader but
    // is not part of the comptime module set (it never enters
    // src/buf_comptime.c). bin/buffalo also builds it on demand when
    // missing; declaring it here keeps the smoke check hermetic and the
    // binary prebuilt for the CLI.
    BuildTarget *tokgen = Executable(ctx, "buf_tokgen");
    SetOutput(tokgen, "buf_tokgen");
    AddSource(tokgen, "tools/buf_tokgen_main.c");
    AddSource(tokgen, "src/buf_tokgen.c");
    AddSource(tokgen, "src/buf_rx.c");
    AddInclude(tokgen, "include/buffalo");
    AddCFlag(tokgen, "-O2");
    AddCFlag(tokgen, "-Wall");

    // Smoke-check the CLI end to end: generate calc's header, then prove the
    // emitted text is a compilable C header.
    BuildTarget *toksmoke = RunCustom(
        ctx, "check-tokens-cli",
        "bin/buffalo tokens examples/calc.bflo -o build/calc_tokens.gen.h && "
        "cc -fsyntax-only -x c build/calc_tokens.gen.h");
    DependsOn(toksmoke, tokgen);
    DependsOn(check, toksmoke);

    // -- digits: hand-written reference demo, plain cc, no cccc ------------
    BuildTarget *digits = Executable(ctx, "digits");
    SetOutput(digits, "digits");
    AddSource(digits, "examples/digits_main.c");
    AddSource(digits, "examples/digits_tables.c");
    AddSource(digits, "runtime/buf_rt.c");
    AddInclude(digits, "runtime");
    AddInclude(digits, "examples");
    AddCFlag(digits, "-O2");
    AddCFlag(digits, "-Wall");
    BuildTarget *cd =
        RunCustom(ctx, "check-digits",
                  "build/digits < examples/digits.txt | "
                  "diff -u examples/digits.expected -");
    DependsOn(cd, digits);
    DependsOn(check, cd);

    // -- generated + native parity, one pair per example --------------------
    int n_lex   = (int)(sizeof(LEX_EXAMPLES) / sizeof(*LEX_EXAMPLES));
    int n_parse = (int)(sizeof(PARSE_EXAMPLES) / sizeof(*PARSE_EXAMPLES));
    for (int e = 0; e < n_lex + n_parse; e++) {
        const char *name = e < n_lex ? LEX_EXAMPLES[e]
                                     : PARSE_EXAMPLES[e - n_lex];
        int         is_parse = e >= n_lex;

        char spec[128], specdef[160], mainc[128], txt[128], expected[128];
        char genc[128], gencmd[1024], stem[64], checkgen[80], checknat[80];
        char parityname[80], genname[80];
        snprintf(spec, sizeof(spec), "examples/%s.bflo", name);
        snprintf(specdef, sizeof(specdef), "\"examples/%s.bflo\"", name);
        snprintf(mainc, sizeof(mainc), "examples/%s_main.c", name);
        snprintf(txt, sizeof(txt), "examples/%s.txt", name);
        snprintf(expected, sizeof(expected), "examples/%s.expected", name);
        if (is_parse) {
            snprintf(genc, sizeof(genc), "build/%s.bflo.parse.gen.c", name);
            snprintf(stem, sizeof(stem), "%s_pgen", name);
            snprintf(genname, sizeof(genname), "gen-%s-parse", name);
        } else {
            snprintf(genc, sizeof(genc), "build/%s.bflo.gen.c", name);
            snprintf(stem, sizeof(stem), "%s_gen", name);
            snprintf(genname, sizeof(genname), "gen-%s", name);
        }

        // --- generated: bin/buffalo lowers the spec, plain cc links it -----
        if (is_parse)
            snprintf(gencmd, sizeof(gencmd), "bin/buffalo parse %s -o %s",
                     spec, genc);
        else
            snprintf(gencmd, sizeof(gencmd), "bin/buffalo lex %s -o %s", spec,
                     genc);
        BuildTarget *gen = RunCustom(ctx, genname, gencmd);
        DeclareOutput(gen, genc);
        add_spec_inputs(gen, spec);

        BuildTarget *ge = Executable(ctx, stem);
        SetOutput(ge, stem);
        AddSourcesGlobDeferred(ge, genc);
        AddSource(ge, mainc);
        AddSource(ge, "runtime/buf_rt.c");
        AddInclude(ge, "runtime");
        AddInclude(ge, "examples");
        AddCFlag(ge, "-O2");
        AddCFlag(ge, "-Wall");
        DependsOn(ge, gen);

        snprintf(checkgen, sizeof(checkgen), "check-%s-generated", stem);
        char gcheckcmd[1024];
        snprintf(gcheckcmd, sizeof(gcheckcmd), "%s < %s | diff -u %s -",
                 TargetOutput(ge), txt, expected);
        BuildTarget *cg = RunCustom(ctx, checkgen, gcheckcmd);
        DependsOn(cg, ge);
        DependsOn(check, cg);

        // --- native: one whole-program cccc --compile=native ---------------
        char nat_stem[64];
        snprintf(nat_stem, sizeof(nat_stem), "%s%s", name,
                 is_parse ? "_pnative" : "_native");
        BuildTarget *nat = CcccExecutable(ctx, nat_stem);
        SetOutput(nat, nat_stem);
        AddSource(nat, "src/buf_comptime.c");
        AddSource(nat, "runtime/buf_rt.c");
        AddSource(nat, mainc);
        AddInclude(nat, "include/buffalo");
        AddInclude(nat, "src");
        AddInclude(nat, "runtime");
        AddInclude(nat, "examples");
        AddDefine(nat, "BUF_SPEC", specdef);
        if (is_parse) {
            AddDefine(nat, "BUF_EMIT_PARSER", (const char *)0);
            AddDefine(nat, "BUF_STOP_AFTER", "7");
        } else {
            AddDefine(nat, "BUF_STOP_AFTER", "5");
        }
        // The .bflo is read at comptime through -D, not #included, so no
        // depfile ever sees it: declare it so a grammar edit invalidates the
        // cached binary. Everything else (token header, comptime modules,
        // runtime headers) is a real #include and lands in cccc's
        // --deps-file tracking on its own.
        AddInput(nat, spec);

        snprintf(checknat, sizeof(checknat), "check-%s", nat_stem);
        char ncheckcmd[1024];
        snprintf(ncheckcmd, sizeof(ncheckcmd), "%s < %s | diff -u %s -",
                 TargetOutput(nat), txt, expected);
        BuildTarget *cn = RunCustom(ctx, checknat, ncheckcmd);
        DependsOn(cn, nat);
        DependsOn(check, cn);

        // generated == native parity. The sandbox shell has no variable
        // expansion or command substitution, and a single command cannot take
        // both a <stdin and a >stdout redirect (cccc shell bug -- noted
        // upstream), so input is piped via cat and the outputs land in files
        // for diff to compare (a diff failure fails the step with real exit
        // codes -- a vacuous pass is impossible).
        snprintf(parityname, sizeof(parityname), "parity-%s", stem);
        char pcmd[1536], pa[128], pb[128];
        snprintf(pa, sizeof(pa), "build/parity-%s.a", stem);
        snprintf(pb, sizeof(pb), "build/parity-%s.b", stem);
        snprintf(pcmd, sizeof(pcmd),
                 "cat %s | %s > %s && cat %s | %s > %s && diff -u %s %s", txt,
                 TargetOutput(ge), pa, txt, TargetOutput(nat), pb, pa, pb);
        BuildTarget *p = RunCustom(ctx, parityname, pcmd);
        DependsOn(p, ge);
        DependsOn(p, nat);
        DependsOn(check, p);

        // three-way parity against the hand-written reference (digits only)
        if (strcmp(name, "digits") == 0) {
            char p3cmd[2048], pr[128], pg[128], pn[128];
            snprintf(pr, sizeof(pr), "build/parity-digits.ref");
            snprintf(pg, sizeof(pg), "build/parity-digits.gen");
            snprintf(pn, sizeof(pn), "build/parity-digits.nat");
            snprintf(p3cmd, sizeof(p3cmd),
                     "cat examples/digits.txt | %s > %s && cat "
                     "examples/digits.txt | %s > %s && cat examples/digits.txt"
                     " | %s > %s && diff -u %s %s && diff -u %s %s",
                     TargetOutput(digits), pr, TargetOutput(ge), pg,
                     TargetOutput(nat), pn, pr, pg, pg, pn);
            BuildTarget *p3 = RunCustom(ctx, "parity-digits", p3cmd);
            DependsOn(p3, digits);
            DependsOn(p3, ge);
            DependsOn(p3, nat);
            DependsOn(check, p3);
        }
    }

    // -- bench: per-phase comptime cost (docs/performance.md). Needs cccc +
    // perl; slow, so it is deliberately not part of check and only declared
    // when asked for:
    //
    //     cccc --build build.c --build-option=bench=1
    if (HaveBuildOption(ctx, "bench"))
        (void)RunCustom(ctx, "bench", "tests/bench.sh");

    return BuildDefault(ctx);
}
