/*
 * Generates synthetic LZO1X test vectors with the reference liblzo2.
 *
 *   gen_vectors OUT_DIR
 *
 * Writes OUT_DIR/NNN.lzo (compressed input), OUT_DIR/manifest.json and the same
 * expectations as tab-separated OUT_DIR/cases.tsv (easier to read from C++).
 * Valid cases come from lzo1x_1 and lzo1x_999 over deterministic synthetic data;
 * their expected output is fully defined by the LZO1X format. Damaged cases
 * (truncated, corrupted, trailing bytes, small output buffer) record what
 * liblzo2's lzo1x_decompress_safe returns; the game uses LZO 2.01, so for those
 * the authoritative answer is the original x86 code, not this library.
 */
#include <lzo/lzo1x.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- deterministic data ---- */

static uint64_t rng_state;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static const char* const kWords[] = {
    "the", "assassin", "templar", "florence", "venice", "ezio", "auditore",
    "rooftop", "guard", "codex", "animus", "memory", "synchronise", "eagle",
    "vista", "hidden", "blade", "notoriety", "courtesan", "thief", "mercenary",
    "\n", ", ", ". ", "1476", "palazzo", "medici", "pazzi", "borgia"};

enum Kind { ZEROS, RANDOM, TEXT, PATTERN, MIXED, RECORDS, NUM_KINDS };
static const char* const kKindNames[] = {"zeros", "random", "text", "pattern",
                                         "mixed", "records"};

static void fill(uint8_t* buf, size_t n, enum Kind kind, uint32_t seed) {
    rng_state = 0x9e3779b97f4a7c15ull ^ ((uint64_t)seed << 32 | (uint64_t)kind);
    size_t i = 0;
    switch (kind) {
    case ZEROS:
        memset(buf, 0, n);
        break;
    case RANDOM:
        for (; i < n; i++) buf[i] = (uint8_t)rnd();
        break;
    case TEXT:
        while (i < n) {
            const char* w = kWords[rnd() % (sizeof kWords / sizeof *kWords)];
            for (; *w && i < n; w++) buf[i++] = (uint8_t)*w;
            if (i < n) buf[i++] = ' ';
        }
        break;
    case PATTERN: {
        size_t period = 1 + rnd() % 37;
        for (; i < n; i++) buf[i] = (uint8_t)(i % period * 7 + 3);
        break;
    }
    case MIXED:
        while (i < n) {
            size_t run = 1 + rnd() % 300;
            if (rnd() & 1) {
                for (size_t k = 0; k < run && i < n; k++) buf[i++] = (uint8_t)rnd();
            } else if (i > 0) {
                size_t dist = 1 + rnd() % (i < 49151 ? i : 49151);
                for (size_t k = 0; k < run && i < n; k++, i++) buf[i] = buf[i - dist];
            } else {
                buf[i++] = 0;
            }
        }
        break;
    case RECORDS: /* game-like: little-endian structs of ids, floats and flags */
        while (i < n) {
            uint32_t rec[8] = {(uint32_t)(i / 32), 0x3f800000u + (rnd() & 0xffff),
                               0x40000000u + (rnd() & 0xff), rnd() & 3, 0, 0xffffffffu,
                               (uint32_t)(i / 32 % 5), 0};
            for (size_t k = 0; k < sizeof rec && i < n; k++) buf[i++] = ((uint8_t*)rec)[k];
        }
        break;
    default:
        break;
    }
}

/* ---- output ---- */

static const char* out_dir;
static FILE* manifest;
static FILE* tsv;
static int case_no;

/* FNV-1a 64, enough to compare outputs; sizes are recorded alongside. */
static uint64_t fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static void emit(const char* kind, const char* note, uint32_t size, const uint8_t* lzo,
                 size_t lzo_len, uint32_t dst_capacity, int valid) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%03d.lzo", out_dir, case_no);
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(lzo, 1, lzo_len, f) != lzo_len || fclose(f)) {
        perror(path);
        exit(1);
    }
    uint8_t* out = malloc(dst_capacity ? dst_capacity : 1);
    lzo_uint out_len = dst_capacity;
    int ret = lzo1x_decompress_safe(lzo, lzo_len, out, &out_len, NULL);
    fprintf(manifest,
            "%s\n    {\"id\": %d, \"file\": \"%03d.lzo\", \"data\": \"%s\", \"case\": \"%s\", "
            "\"uncompressed_size\": %u, \"input_size\": %zu, \"dst_capacity\": %u, "
            "\"valid\": %s, \"expect_ret\": %d, \"expect_out_len\": %lu, "
            "\"expect_out_fnv64\": \"%016llx\"}",
            case_no ? "," : "", case_no, case_no, kind, note, size, lzo_len, dst_capacity,
            valid ? "true" : "false", ret, (unsigned long)out_len,
            (unsigned long long)fnv64(out, ret == LZO_E_OK || out_len <= dst_capacity
                                               ? out_len : 0));
    fprintf(tsv, "%d\t%03d.lzo\t%d\t%u\t%zu\t%d\t%lu\t%016llx\n", case_no, case_no, valid,
            dst_capacity, lzo_len, ret, (unsigned long)out_len,
            (unsigned long long)fnv64(out, ret == LZO_E_OK || out_len <= dst_capacity
                                               ? out_len : 0));
    if (valid && ret != LZO_E_OK) {
        fprintf(stderr, "case %d: reference failed to decode a valid stream (%d)\n",
                case_no, ret);
        exit(1);
    }
    free(out);
    case_no++;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUT_DIR\n", argv[0]);
        return 2;
    }
    if (lzo_init() != LZO_E_OK) return 1;
    out_dir = argv[1];
    char path[1024];
    snprintf(path, sizeof path, "%s/manifest.json", out_dir);
    manifest = fopen(path, "w");
    if (!manifest) {
        perror(path);
        return 1;
    }
    snprintf(path, sizeof path, "%s/cases.tsv", out_dir);
    tsv = fopen(path, "w");
    if (!tsv) {
        perror(path);
        return 1;
    }
    fprintf(tsv, "# id\tfile\tvalid\tdst_capacity\tinput_size\texpect_ret\texpect_out_len\t"
                 "expect_out_fnv64\n");
    fprintf(manifest, "{\n  \"generator\": \"tests/lzo/gen_vectors.c\",\n"
                      "  \"reference_lzo_version\": \"%s\",\n"
                      "  \"note\": \"valid cases are format-defined; damaged cases record "
                      "liblzo2 behaviour, the game uses LZO 2.01\",\n  \"cases\": [",
            lzo_version_string());

    static const uint32_t kSizes[] = {0, 1, 2, 3, 4, 15, 16, 17, 255, 256, 4096,
                                      65535, 65536, 262144};
    static uint8_t wrk[LZO1X_999_MEM_COMPRESS];
    for (int kind = 0; kind < NUM_KINDS; kind++) {
        for (size_t si = 0; si < sizeof kSizes / sizeof *kSizes; si++) {
            uint32_t n = kSizes[si];
            /* Large incompressible inputs only add bulk; keep one per kind. */
            if (n == 262144 && kind == RANDOM) continue;
            uint8_t* src = malloc(n ? n : 1);
            fill(src, n, (enum Kind)kind, n);
            size_t cap = n + n / 16 + 64 + 3;
            uint8_t* lzo = malloc(cap);
            for (int level = 0; level < 2; level++) {
                lzo_uint lzo_len = cap;
                int r = level == 0 ? lzo1x_1_compress(src, n, lzo, &lzo_len, wrk)
                                   : lzo1x_999_compress(src, n, lzo, &lzo_len, wrk);
                if (r != LZO_E_OK) return 1;
                emit(kKindNames[kind], level == 0 ? "lzo1x_1" : "lzo1x_999", n, lzo,
                     lzo_len, n, 1);

                /* Damaged variants, from the 999 stream of mid-sized inputs. */
                if (level == 1 && n >= 256 && n <= 65536) {
                    emit(kKindNames[kind], "truncated_half", n, lzo, lzo_len / 2, n, 0);
                    emit(kKindNames[kind], "truncated_minus1", n, lzo, lzo_len - 1, n, 0);
                    emit(kKindNames[kind], "dst_too_small", n, lzo, lzo_len, n - 1, 0);
                    uint8_t* bad = malloc(lzo_len + 4);
                    memcpy(bad, lzo, lzo_len);
                    rng_state = 0x1234567ull + (uint64_t)case_no;
                    for (int k = 0; k < 3; k++) bad[rnd() % lzo_len] ^= (uint8_t)(1 + rnd() % 255);
                    emit(kKindNames[kind], "corrupted", n, bad, lzo_len, n, 0);
                    memcpy(bad, lzo, lzo_len);
                    memcpy(bad + lzo_len, "\x11\x00\x00\x00", 4);
                    emit(kKindNames[kind], "trailing_bytes", n, bad, lzo_len + 4, n, 0);
                    free(bad);
                }
            }
            free(lzo);
            free(src);
        }
    }
    fprintf(manifest, "\n  ]\n}\n");
    fclose(manifest);
    fclose(tsv);
    fprintf(stderr, "wrote %d cases to %s\n", case_no, out_dir);
    return 0;
}
