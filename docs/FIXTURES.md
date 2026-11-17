# Fixtures

Small functions cut from the game with `tools/extract_fixture.py`, used to test
the recompiler without the game installed. Only allow-listed open-source library
code (LZO, zlib) up to 16 KB per function may be extracted.

## ac2-1.0.1 (from AC2 `scimitar_sf`, SHA-1 `afcbbbf8…f15e5`)

The game statically links **LZO 2.01** (`lzo_version()` returns `0x2010`). LZO is
Markus F.X.J. Oberhumer's library, licensed under the GNU GPL version 2 or later;
these fixtures are its compiled code, distributed under the same terms as the rest
of this repository (see [LICENSE](../LICENSE)). LZO's source is at
<https://www.oberhumer.com/opensource/lzo/>.

| Fixture | Load address | Size | Instructions | Calls / indirect / data refs |
|---|---|---|---|---|
| `lzo1x_decompress_safe` | `0x0015c1c0` | 1344 | 439 | none |
| `lzo1x_decompress` | `0x0015dc70` | 912 | 300 | none |

Each `.bin` must be placed at its load address: relative branch targets inside it
depend on that. Each `.json` records the address, size, code hash, source binary
hash and an instruction summary.

### Calling convention (i386 cdecl)

```c
int lzo1x_decompress_safe(const uint8_t* src, uint32_t src_len,
                          uint8_t* dst, uint32_t* dst_len, void* wrkmem);
```

- Arguments on the guest stack at `[esp+4]` … `[esp+20]` after the call pushes the
  return address; result in `eax`.
- `*dst_len` holds the output capacity on entry and the bytes written on return.
- `wrkmem` is unused by LZO1X decompression (pass 0).
- Return codes: `0` OK, `-1` error, `-4` input overrun, `-5` output overrun,
  `-6` lookbehind overrun, `-7` EOF not found, `-8` input not consumed.
- `lzo1x_decompress` (unchecked) must only be given valid streams with enough
  output space.

## Running a fixture in an emulator (oracle)

Map guest memory, copy the `.bin` to its load address, place `src`, `dst` and the
`dst_len` word, push the five arguments and a sentinel return address, set `eip`
to the load address and run until `eip` equals the sentinel.
