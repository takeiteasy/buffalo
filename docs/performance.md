# Performance — the M3 comptime-cost spike

buffalo runs its whole front half (spec read, token-header check, Thompson
NFA, subset-construction DFA) and a minimal emit step **inside cccc's comptime
VM**, at the compile time of whatever program includes the generated lexer. M3
is the gate that asked whether that is affordable before committing to the M4
emitter. This is the answer, with the method so it can be re-run.

## Verdict — GO, with a known ceiling

For a realistic ~45-rule C-like lexer (`examples/clike.l`), running the entire
pipeline **including emission** adds **~0.38 s** of compile time over a no-op
comptime baseline. Threshold was < 1 s. **M4 proceeds.**

The spike measurement went through one round of optimisation first. The
initial numbers exposed a superlinear blow-up in DFA construction — a 103-rule
/ 324-state stress spec (`examples/big.l`) added **~15 s**. Two fixes in
`buf_dfa.h` (below) cut that to **~2.25 s** and roughly halved `clike.l`. The
cost is still superlinear in DFA state count and still 100 % in the DFA phase,
so the ceiling is explicit:

| spec | rules | DFA states | added compile time |
|---|---|---|---|
| `calc.l` | 9 | 13 | +0.025 s |
| `clike.l` | 45 | 85 | **+0.38 s** |
| `big.l` | 103 | 324 | +2.25 s |

A lexer up to `clike.l`'s size is comfortably affordable. Interpolating
between the measured 85-state (+0.38 s) and 324-state (+2.25 s) points, ~150
DFA states lands near the 1 s gate; a 100+-rule grammar in the 300-state
range is past it. Emission is free at every size — raw `GlobalVarSetInitData`
blobs plus one `Quote()`d wrapper, lost in the noise.

DFA minimisation does **not** move this ceiling, and the opt-in Moore pass
(next section) measures why.

Only `calc.l` and `clike.l` run through the comptime VM in `make check` (the
`spec` target); `big.l`'s comptime path is reachable through
`tests/bench.sh examples/big.l` only, kept out of `check` for its ~2.5 s.

## Method

`src/buf_comptime.c` carries an ablation ladder, selected with
`-D BUF_STOP_AFTER=n`:

| n | stops after |
|---|---|
| 0 | nothing (no-op comptime baseline) |
| 1 | + spec read |
| 2 | + token-header validation |
| 3 | + NFA construction |
| 4 | + DFA construction |
| 5 | + emit (default) |

`-D BUF_STATS` prints arena peaks to stderr after the DFA build;
`-D BUF_DFA_STATS` (in `buf_dfa.h`) adds closure-call and state-set-compare
counters. `-D BUF_MINIMIZE` runs the opt-in Moore minimisation pass after
subset construction (see "DFA minimisation" below). `tests/bench.sh` walks the
ladder for each spec, N reps, and reports the median wall time and the
per-phase delta, plus a `4 +DFA+min` row for the minimisation cost:

```sh
make bench                     # calc.l + clike.l
REPS=9 tests/bench.sh           # more reps
tests/bench.sh examples/big.l   # the stress spec (slow)
```

`tests/t_dfa.c` prints the **native** per-phase timing (host `cc`, no cccc) at
the end of its run — the control that separates algorithm cost from VM
interpretation cost.

## Numbers

aarch64-darwin (M1), cccc 0.1.0, `-O2`. No-op comptime baseline ≈ 0.31 s.
`calc.l`/`clike.l` are 7-rep medians; `big.l` is a 3-rep median.

### Per-phase added compile time (post-optimisation)

| phase | `calc.l` | `clike.l` | `big.l` |
|---|---|---|---|
| spec read | +0.010 | +0.008 | +0.026 |
| + token check | ~0 | +0.001 | +0.004 |
| + NFA | ~0 | +0.005 | +0.015 |
| + DFA | **+0.021** | **+0.361** | **+2.223** |
| + emit | +0.002 | +0.003 | −0.043 (noise) |
| **total added** | **+0.025** | **+0.38** | **+2.25** |

Read / token-check / NFA are at or below the run-to-run noise floor (±10 ms).
The DFA phase is the entire signal.

### Native control (host `cc`, per build)

| spec | read | NFA | DFA (pre-opt) | DFA (post-opt) |
|---|---|---|---|---|
| `calc.l` | 0.03 ms | ~0 | 0.043 ms | 0.034 ms |
| `clike.l` | 0.05 ms | ~0 | 0.760 ms | 0.42 ms |
| `big.l` | 0.05 ms | ~0 | — | 1.78 ms |

The comptime VM runs the DFA phase **~1000–1300× slower than native**
(`big.l`: 1.8 ms native, ~2.25 s comptime). The DFA phase does by far the most
interpreted statement executions of any phase — the `nstates × nclass`
transition fill, each step scanning the state's NFA-index set and taking an
ε-closure — so it absorbs the interpretation multiplier hardest. Everything
upstream (regex parse, NFA build) is small and stays in the noise.

### The optimisation round

The first measurement had `clike.l` at +0.73 s and `big.l` at ~+15 s, driven
by two spots in `buf_dfa.h` that are quadratic-ish in state count and were
harmless until the VM multiplier made them visible:

| fix | before → after (`big.l`) |
|---|---|
| **`buf_dfa_find`** — was a linear scan of every DFA state per new-state lookup; now an FNV-1a hash index (`hash_head` / `hash_next`) over the sorted state-set run. | ~1,001,000 → ~11,400 set-compares |
| **`buf_dfa_closure`** — cleared and re-swept `mark[0..N_nfa)` on every one of ~11,700 calls; now a generation counter (`mark_gen`, bumped not cleared) plus an insertion sort of the small closure worklist instead of a full 0..N_nfa sweep. | one O(N_nfa) clear + one O(N_nfa) collect per call → O(closure size) |

Net: `big.l` DFA phase ~13 s → ~2.25 s (~6×); `clike.l` +0.73 s → +0.38 s.
The determinism guarantee (byte-identical tables across builds — needed for
M5's generated/native parity) is unchanged and is regression-tested on
`big.l`'s 324-state DFA in `tests/t_dfa.c`.

What is left in `big.l`'s 2.25 s is spread across `buf_dfa_partition`
(`node_count × nclass × 256` refinement sweep), the ~11,700 structural closure
calls, and the subset loop's set scans — no single remaining hot spot.

## DFA minimisation — measured, kept opt-in

The obvious next lever looked like cutting the DFA state count itself with
Hopcroft/Moore minimisation. It was implemented (Moore partition refinement,
`buf_dfa_minimize` in `buf_dfa.h`, gated by `-D BUF_MINIMIZE`) and measured.
The result is that it is **not worth running by default**:

State count and `.gen.c` size are the `-D BUF_MINIMIZE` build vs. the default;
the comptime column is the `+DFA` phase delta (`bench.sh`'s `d(phase)`) with
minimisation off, then on:

| spec | DFA states | `.gen.c` bytes | `+DFA` phase | `+DFA` with `-D BUF_MINIMIZE` |
|---|---|---|---|---|
| `calc.l` | 13 → 11 | 2221 → 2134 | +0.021 s | +0.021 s |
| `clike.l` | 85 → 78 | 16788 → 15475 | +0.374 s | +0.390 s |
| `big.l` | 324 → 310 | 102745 → 97621 | +2.19 s | **+2.52 s** |

Two things sink it:

1. **Subset construction already lands near the minimal DFA.** A lexer's DFA
   is pre-split by winning rule (`accept[s]` is a distinct value per rule, so
   `big.l` starts refinement with ~103 blocks), and the only merges left are
   non-accepting prefix states and identical rule tails — 4–8 % of the states.
2. **Minimisation runs *after* the expensive phase and adds to it.** It is an
   `O(passes · nstates · nclass)` pass over the finished DFA, interpreted at
   the same ~1000× VM multiplier, so on `big.l` it *adds* ~0.33 s to the phase
   it was meant to relieve. The `.gen.c` shrinks ~5 %, but downstream stock
   `cc -O2 -c` on that file is ~0.03 s either way — no measurable win there.

So the pass ships **off by default**. `buffalo lex … -- -D BUF_MINIMIZE` (or
`buf_dfa_build_ex(dfa, nfa, rx, 1)` from host code) turns it on for the cases
that want the smaller table and can spend the compile time; `tests/bench.sh`
reports its added cost as the `4 +DFA+min` row. Cutting `big.l`'s 2.25 s for
real means the `buf_dfa_partition` sweep, not minimisation.

### Arena peaks

| arena | cap | `clike.l` | `big.l` |
|---|---|---|---|
| regex AST nodes | 4096 | 162 | 739 |
| rules | 256 | 45 | 103 |
| NFA states | 8192 | 306 | 1077 |
| DFA states | 4096 | 85 | 324 |
| alphabet classes | 256 | 43 | 76 |
| transition table (`nstates·nclass`) | 262144 | 3655 | 24624 |
| state-set pool | 65536 | 477 | 2157 |

`big.l` at 103 rules first exposed `BUF_RX_MAX_RULES` at its old value of 128;
it is now 256, matching `BUF_RX_MAX_TOKENS` (every `%tokens` entry needs a
rule, plus the `%skip` rules — so the rule cap must be `>=` the token cap).
Every arena is 2.5–140× under its cap, and the caps stay generous: they are
fixed-size `static` arrays and compile time tracks the live counts, not the
caps, so shrinking one saves comptime-host `.bss` and nothing else. Note
`BUF_DFA_MAX_TRANS` is `BUF_DFA_MAX_STATES * 64`.

### Generated-file size

| spec | DFA states × classes | `.gen.c` |
|---|---|---|
| `calc.l` | 13 × 11 | 2.2 KB |
| `clike.l` | 85 × 43 | 16.8 KB |
| `big.l` | 324 × 76 | 100 KB |

cccc serialises each raw init blob as a C string literal (`"\000\001\002…"`),
so the source text runs ~2× the raw table bytes. `clike.l`'s tables are
~7.6 KB raw; the ticket's "~4–10 KB of tables vs. ~100 KB unclassed" estimate
holds for the table bytes at that size (the 43-class alphabet collapse is
doing its job). `big.l` shows the file scaling roughly with `nstates·nclass`,
as expected — a 300-state generated lexer is a ~100 KB file, most of it the
`next` table.

## Known wrinkle

Every cccc `-c=generated` file carries one dead `BufToken __cccc_tmp0;` local
in the `buf_next` wrapper, which a warning build flags as unused. It is cccc
codegen, not buffalo output; the `Makefile` scopes `-Wno-unused-variable` to
the `.gen.c` translation unit (`GEN_CFLAGS`). Emitted-table *correctness*
(blob byte order, the `char` class table) is pinned by `make native`'s
three-way diff — hand-written `build/digits` == generated == one-shot native
— the only end-to-end check of cccc's string-literal blob round-trip.
