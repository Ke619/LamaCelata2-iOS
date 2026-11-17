# recomp

## Tools

```bash
# One self-contained function from raw bytes (fixtures):
build/scimitar-recomp --bin CODE.bin --addr 0xADDR --name NAME --out OUT.cpp

# Functions from the game executable plus everything they call (roots are symbols
# or 0xADDR; a roots file has one per line); --parts N splits the output:
build/scimitar-recomp --image scimitar_sf --root SYMBOL... [--roots-file FILE] \
    [--override SYMBOL]... [--no-vtables] [--parts N] --out OUT.cpp
# --weak-imports: a weak stub that calls fatal() for every import (real shims win).
# Every function of the image (functions that cannot be translated become stubs):
build/scimitar-recomp --image scimitar_sf --all --parts 64 --out build/gen/full/ac2.cpp

# Call closure report (what translating some functions would involve):
build/scimitar-analyze scimitar_sf ROOT_SYMBOL... [--stop NAME_OR_PREFIX]... [--list]
# One function, with call targets named (address inside it, or symbol):
build/scimitar-analyze scimitar_sf --disasm 0xADDRESS|SYMBOL...
# Every function that calls or tail-calls the named ones:
build/scimitar-analyze scimitar_sf --callers SYMBOL...
# Whole-image progress: how many functions translate on their own, and the first
# blocker of the others (--list-failures: every failing function;
# --list-indirect: indirect jumps that are neither jump tables nor tail calls):
build/scimitar-analyze scimitar_sf --coverage [--list-failures | --list-indirect]
# Functions for tests/difftest (closure translatable, only allowed imports):
build/scimitar-analyze scimitar_sf --difftest-candidates [--max N] [--max-closure K] \
    [--allow-import DYLD_NAME]... [--out FILE]
```

Image-mode output contains translated game code: it goes to `build/gen/`, never to git.

## How it works

- `macho.cpp` reads the i386 Mach-O: sections, function extents from the symbol
  table, import stubs and pointer slots from the indirect symbol table (stubs bound
  to the image's own weak definitions resolve to those functions), and applies
  external relocations (Brotherhood's linker emits one for every reference to a weak
  definition).
- `pe.cpp` reads PE32 executables (the Windows ports): sections, imports by name and
  ordinal, base relocations. With no symbols, functions are found by recursive
  descent from the entry, exports, RTTI vtables, relocations and code addresses in
  data; `--override` takes `sub_XXXXXXXX` names there.
- `flow.cpp` decodes a function with Zydis by following control flow from its
  entry, and classifies each transfer: internal jump, tail call, call, import call,
  `call next` (PIC base), indirect call, jump table, other indirect jump. Jump
  tables are recognised in GCC's three forms (`jmp *T(,%i,4)`,
  `lea (,%i,4),%r; jmp *T(%r)`, and PIC-relative `mov K(%pic,%i,4),%r; add %pic,%r;
  jmp *%r` with `%pic` set by `call next; pop`); the entry count comes from the
  `cmp imm; ja`/`jbe` bounds check (found through straight-line and branch
  predecessors), or else the table ends at the first entry outside the function.
  MSVC's forms are recognised too: two-level switches with a byte index table,
  masked indexes, `mov T(,%i,4),%r; jmp *%r`, and the CRT `memcpy`'s tables. Their targets are decoded like any branch target. The compiler emits nothing
  after calls that never return (or only `nop` padding), so falling into the next
  function right after a call ends the function quietly; the generated code calls
  `fatal` if execution ever gets there.
- `discover_functions` (run by every tool) splits a symbol's extent where a direct
  call or a jump out of a function lands inside it, or where code or data (static
  initializer and callback tables) holds the address of a spot its container never
  reaches that starts with the game's prologue (`push %ebp; mov %esp, %ebp`): code
  without a symbol, such as stripped static helpers in the statically linked
  libstdc++ (46 in AC2). Functions in `__TEXT,__StaticInit` (static initializers
  without symbols) come from `__mod_init_func`.
- `emit.cpp` emits `void f_XXXXXXXX(scimitar::Context& c)` per function:
  - registers in `c`, guest memory via `M = c.mem->base()` and `rd*/wr*`;
  - internal branches as `goto`;
  - calls push the real return address on the guest stack, then call `f_target(c)`;
    the callee's `ret` pops it. Tail calls become `f_target(c); return;`;
  - import calls go to `scimitar::imports::imp_*` shims (platform layer);
  - indirect calls go through `call_indirect`, which looks up the registry of
    translated functions (each generated file registers its functions) and of
    native host functions;
  - a jump table computes its target as the original code does and dispatches
    with a C++ `switch` over the table's labels (any other target is fatal);
  - other indirect jumps are tail calls through a pointer (`call_indirect`);
  - flags are local booleans kept only where a later instruction reads them;
    calls clobber flags; immediates are sized by the operand width.
- Each emitted block carries the original instruction as a comment.
- Image mode follows vtables: when translated code refers to an address inside a
  `__ZTV…` symbol (typically a constructor installing it), every function that
  vtable points to joins the closure. Likewise a function whose address the code
  takes (callbacks, `pthread_once` routines, thread entries). References count when
  they are absolute, relative to a PIC base (`call next; pop`, in the bundled
  libstdc++), or through a pointer slot bound to the image's own weak definition (how
  vtables of inline classes are reached). Those functions may hold code the translator
  does not support yet; they become stubs that call `fatal` if ever executed. Anything
  reachable from the roots through direct calls must translate, or the build fails.

Supported: `mov movzx movsx lea push pop leave ret jmp call`, all `jcc`/`setcc`/`cmovcc`,
`add adc sub sbb cmp and or xor test inc dec neg not shl shr sar`, `mul`, `imul` (1, 2
and 3 operands), `div`/`idiv` (32-bit; a zero divisor or overflow is fatal, as x86
faults), `nop` forms, jump tables, and the x87 instructions the game uses (`fld`/`fst`/`fstp`
for m32/m64/m80 and registers, `fild`/`fist`/`fistp`/`fisttp`, `fldz`, `fld1`,
`fadd`/`fsub`/`fsubr`/`fmul`/`fdiv`/`fdivr` with their popping and integer forms,
`fxch`, `fchs`, `fabs`, `fcmovcc`, `fldcw`/`fnstcw`; and, for MSVC code and its CRT,
the compares to flags and the status word, `fsqrt`, `frndint`, `fsin`/`fcos`/
`fsincos`/`fptan`/`fpatan`, `fprem`, `f2xm1`/`fyl2x`/`fscale`, the constant loads, and
the environment and state instructions). x87 registers hold exact 80-bit
values (`runtime/src/x87.cpp`): conversions in are exact, stores and arithmetic round as
the control word says, and `tests/x87` checks 450,000 results against Unicorn with no
difference (apart from two places where QEMU differs from real hardware: fisttp
overflow and signalling-NaN conversions; we follow the hardware, and a test-only
QEMU-compatible mode matches Unicorn exactly).

SSE/SSE2/SSE3 as the game uses it (scalar and packed single/double arithmetic,
min/max, sqrt, rcp/rsqrt, compares to masks and to flags, logic, shuffles and
unpacks, conversions, moves of every width, SSE2 integer add/compare/shift/unpack,
`ldmxcsr`/`stmxcsr`), through `runtime/include/scimitar/sse.h`. Results are x86's on
any host: invalid operations give the negative default NaN and NaN operands
propagate as on x86 (ARM's default NaN is positive), float-to-int conversions give
`0x80000000` instead of saturating, min/max follow x86's operand rule. rcp/rsqrt are
exact (as in Unicorn); real CPUs return 12-bit approximations. `ldmxcsr` applies the
rounding mode and flush-to-zero to the host FPU. MMX (only Wwise's sample converters
use it) aliases the x87 registers as on hardware: `mmN` is the significand of physical
register N, MMX instructions set TOP to 0 and every tag valid, `emms` empties them.

Also: string instructions (`movs`/`stos`/`lods`/`cmps`/`scas` with `rep`/`repe`/`repne`
and the direction flag), `lock` forms of `add`/`sub`/`and`/`or`/`xor`/`inc`/`dec`/`xadd`
and `xchg` with memory as host atomics (sequentially consistent), rotates, `shld`/`shrd`,
`bswap`, `bsf`/`bsr`, `cwde`/`cbw`/`cdq`/`cwd`, 8/16-bit `mul`/`imul`/`div`/`idiv`,
`rdtsc` (a 1 GHz counter from host time), `cpuid` (a fixed SSE3-class CPU), and
`int3`/`hlt`/`ud2` as fatal errors.

## Results so far

| Code | Functions / instructions | Result |
|---|---|---|
| `lzo1x_decompress_safe` fixture | 1 / 431 | 286/286 vectors identical to the original x86 |
| AC2 forge tables, file read and decompression (`tests/forge`) | 93 / 4142, 5 imports | identical to the original x86 on the real `DataMAC.forge`; decompressed data identical to `liblzo2` |
| AC2 differential test (`tests/difftest`): 1500 game functions chosen by `--difftest-candidates` to cover every instruction kind (about 80 SSE mnemonics, x87 arithmetic, jump tables, string, locked and bit instructions), generated inputs, 8 cases each; registers, x87 state and writable memory compared | 2276 / 72169 | 8889 of 8889 cases run identical to Unicorn (1294 functions; 206 had no case that ran: inputs faulted or left the closure); the 4 MMX converters identical in every case that ran |

Whole image, compiled (`cmake -DSCIMITAR_FULL_IMAGE=ON`, target `ac2_full`): every
function translated into 64 files (947 MB of C++, 39.9M lines, 12 s), which compile
without errors in 125 s on 10 cores at `-O1` (204 MB of objects; at most 1.24 GB of
memory and 53 s for one file; the ports now use `-O2`). The ports link it with the
platform layer (`build/ac2`, `build/acb`; the top README).

Whole image (`--coverage`): all 208,782 functions translate on their own (7,895,140
instructions control flow reaches). 799 jump tables resolve; 5,413
other indirect jumps are tail calls through a pointer.
