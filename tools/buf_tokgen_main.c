/*
 * buf_tokgen_main.c -- main() for the buf_tokgen token-header generator.
 *
 * Host-only tool, plain `cc` (no cccc): reads a .bflo spec with buf_rx,
 * renders its %tokens list as a <name>_tokens.h header (buf_tokgen.h), and
 * writes it out. `bin/buffalo tokens` execs this binary, building it on
 * demand the first time; build.c also declares it as a target so the CLI
 * smoke check is hermetic.
 *
 *     buf_tokgen SPEC.bflo [-o PATH]
 *
 * The default output path mirrors src/buf_comptime.c's token-header
 * derivation: SPEC with a trailing ".bflo" replaced by "_tokens.h".
 */
#include <stdio.h>
#include <string.h>

#include "buf_rx.h"
#include "buf_tokgen.h"

static void usage(void) {
    fprintf(stderr, "usage: buf_tokgen SPEC.bflo [-o PATH]\n");
}

/* SPEC.bflo -> SPEC_tokens.h (trailing ".bflo" replaced, otherwise appended)
 * -- the same rule src/buf_comptime.c applies when BUF_TOKENS_H is absent. */
static void derive_out_path(char *dst, int cap, const char *spec) {
    size_t n      = strlen(spec);
    size_t extlen = strlen(".bflo");
    size_t soff   = strlen("_tokens.h");
    size_t base   = (n >= extlen && strcmp(spec + n - extlen, ".bflo") == 0)
                        ? n - extlen
                        : n;
    if (base + soff >= (size_t)cap) base = (size_t)cap - soff - 1;
    memcpy(dst, spec, base);
    memcpy(dst + base, "_tokens.h", soff + 1);
}

int main(int argc, char **argv) {
    static BufRx rx; /* ~megabyte-scale fixed arenas; keep off the stack */
    static BufTg tg;
    char        outbuf[512];
    const char *spec = NULL, *out = NULL;
    FILE       *f;
    int         i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "buf_tokgen: -o needs a path\n");
                usage();
                return 2;
            }
            out = argv[++i];
        } else if (spec) {
            fprintf(stderr, "buf_tokgen: unexpected argument '%s'\n", argv[i]);
            usage();
            return 2;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "buf_tokgen: unexpected argument '%s'\n", argv[i]);
            usage();
            return 2;
        } else {
            spec = argv[i];
        }
    }
    if (!spec) {
        fprintf(stderr, "buf_tokgen: missing SPEC.bflo\n");
        usage();
        return 2;
    }
    if (!out) {
        derive_out_path(outbuf, sizeof(outbuf), spec);
        out = outbuf;
    }

    if (buf_rx_read_file(&rx, spec) != 0) {
        fprintf(stderr, "buf_tokgen: %s\n", rx.error);
        return 1;
    }
    if (buf_tokgen_emit(&tg, &rx) != 0) {
        fprintf(stderr, "buf_tokgen: %s\n", tg.error);
        return 1;
    }

    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "buf_tokgen: cannot open '%s' for writing\n", out);
        return 1;
    }
    if (fwrite(tg.out, 1, (size_t)tg.out_len, f) != (size_t)tg.out_len ||
        fclose(f) != 0) {
        fprintf(stderr, "buf_tokgen: write to '%s' failed\n", out);
        remove(out);
        return 1;
    }
    printf("Token header written to %s\n", out);
    return 0;
}
