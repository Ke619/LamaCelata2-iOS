/* Synthetic workloads for measuring how fast translated code runs on a host
 * (tests/bench/README.md). Written to resemble what a game engine's frame
 * does (vector math, particles, hashing, sorting, entity updates through
 * function tables, tree walks, text parsing), with no game code in them.
 *
 * The same file is built twice:
 * - as an i386 Windows executable (MinGW GCC, SSE floating point like the
 *   game's compiler), which scimitar-recomp translates; the bench runs the
 *   translated functions on the host;
 * - natively for the host, for comparison.
 * Each kernel returns a checksum; both builds must agree. Links between nodes
 * are 32-bit indices, so the data has the same layout in every build. */
#include <stdint.h>

#if defined(BENCH_GUEST)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
#define NOINLINE __attribute__((noinline))

/* No C library in the guest build: the few helpers it would call. */
#if defined(BENCH_GUEST)
void* memset(void* d, int c, unsigned n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void* memcpy(void* d, const void* s, unsigned n) {
    uint8_t* p = (uint8_t*)d;
    const uint8_t* q = (const uint8_t*)s;
    while (n--) *p++ = *q++;
    return d;
}
int bench_entry(void) { return 0; }
#endif

static uint32_t rng_next(uint32_t* s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}
static float rng_float(uint32_t* s) { return (float)(rng_next(s) >> 8) * (1.0f / 16777216.0f); }
static uint32_t fbits(float f) {
    union { float f; uint32_t u; } v;
    v.f = f;
    return v.u;
}
static uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

/* ---- vector math: transforms, lighting, normalisation ---- */
typedef struct { float x, y, z, w; } vec4;
typedef struct { float m[16]; } mat4;

/* Small float functions that are not inlined return in x87 ST0 on i386. */
NOINLINE static float dot3(const vec4* a, const vec4* b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
NOINLINE static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void mat_mul(mat4* r, const mat4* a, const mat4* b) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a->m[i * 4 + k] * b->m[k * 4 + j];
            r->m[i * 4 + j] = s;
        }
}

static vec4 transform(const mat4* m, vec4 v) {
    vec4 r;
    r.x = m->m[0] * v.x + m->m[4] * v.y + m->m[8] * v.z + m->m[12] * v.w;
    r.y = m->m[1] * v.x + m->m[5] * v.y + m->m[9] * v.z + m->m[13] * v.w;
    r.z = m->m[2] * v.x + m->m[6] * v.y + m->m[10] * v.z + m->m[14] * v.w;
    r.w = m->m[3] * v.x + m->m[7] * v.y + m->m[11] * v.z + m->m[15] * v.w;
    return r;
}

EXPORT uint32_t bench_vecmath(uint8_t* scratch, uint32_t reps) {
    enum { kVerts = 2048, kBones = 32 };
    vec4* pos = (vec4*)scratch;
    vec4* nrm = pos + kVerts;
    vec4* out = nrm + kVerts;
    mat4* bones = (mat4*)(out + kVerts);
    uint32_t s = 12345;
    for (int i = 0; i < kVerts; i++) {
        pos[i].x = rng_float(&s) * 10 - 5, pos[i].y = rng_float(&s) * 10 - 5;
        pos[i].z = rng_float(&s) * 10 - 5, pos[i].w = 1;
        nrm[i].x = rng_float(&s) - 0.5f, nrm[i].y = rng_float(&s) - 0.5f;
        nrm[i].z = rng_float(&s) - 0.5f, nrm[i].w = 0;
    }
    for (int b = 0; b < kBones; b++)
        for (int k = 0; k < 16; k++) bones[b].m[k] = (k % 5 == 0 ? 1.0f : 0.0f) + (rng_float(&s) - 0.5f) * 0.1f;
    vec4 light = {0.577f, 0.577f, 0.577f, 0};
    float acc = 0;
    for (uint32_t r = 0; r < reps; r++) {
        mat4 view;
        mat_mul(&view, &bones[r % kBones], &bones[(r + 7) % kBones]);
        for (int i = 0; i < kVerts; i++) {
            vec4 p = transform(&view, pos[i]);
            vec4 n = transform(&bones[i % kBones], nrm[i]);
            const float len2 = dot3(&n, &n);
            const float inv = len2 > 1e-12f ? 1.0f / __builtin_sqrtf(len2) : 0.0f;
            n.x *= inv, n.y *= inv, n.z *= inv;
            const float lit = clampf(dot3(&n, &light), 0.0f, 1.0f);
            const float iw = p.w != 0 ? 1.0f / p.w : 1.0f;
            out[i].x = p.x * iw, out[i].y = p.y * iw, out[i].z = p.z * iw, out[i].w = lit;
            acc += out[i].x * 0.001f + lit;
        }
    }
    return fbits(acc);
}

/* ---- particles: integration, collisions, grid binning ---- */
typedef struct { float px, py, pz, vx, vy, vz, life; uint32_t cell; } particle;

EXPORT uint32_t bench_particles(uint8_t* scratch, uint32_t reps) {
    enum { kCount = 4096, kGrid = 32 };
    particle* p = (particle*)scratch;
    uint32_t* grid = (uint32_t*)(p + kCount);
    uint32_t s = 777;
    for (int i = 0; i < kCount; i++) {
        p[i].px = rng_float(&s) * 64, p[i].py = rng_float(&s) * 20, p[i].pz = rng_float(&s) * 64;
        p[i].vx = rng_float(&s) - 0.5f, p[i].vy = rng_float(&s) * 2, p[i].vz = rng_float(&s) - 0.5f;
        p[i].life = rng_float(&s) * 5, p[i].cell = 0;
    }
    const float dt = 1.0f / 30.0f, cx = 32, cz = 32, radius = 8;
    uint32_t h = 0;
    for (uint32_t r = 0; r < reps; r++) {
        for (int g = 0; g < kGrid * kGrid; g++) grid[g] = 0;
        for (int i = 0; i < kCount; i++) {
            particle* q = &p[i];
            q->vy -= 9.81f * dt;
            q->vx *= 0.995f, q->vz *= 0.995f;
            q->px += q->vx * dt, q->py += q->vy * dt, q->pz += q->vz * dt;
            if (q->py < 0) {  /* ground bounce */
                q->py = -q->py;
                q->vy = -q->vy * 0.6f;
            }
            const float dx = q->px - cx, dz = q->pz - cz;
            const float d2 = dx * dx + dz * dz;
            if (d2 < radius * radius && d2 > 1e-6f) {  /* pushed out of a pillar */
                const float d = __builtin_sqrtf(d2), k = (radius - d) / d;
                q->px += dx * k, q->pz += dz * k;
                q->vx = -q->vx, q->vz = -q->vz;
            }
            q->life -= dt;
            if (q->life <= 0) {
                q->px = rng_float(&s) * 64, q->py = 10, q->pz = rng_float(&s) * 64;
                q->vy = rng_float(&s) * 2, q->life = 5;
            }
            int gx = (int)(q->px * (kGrid / 64.0f)), gz = (int)(q->pz * (kGrid / 64.0f));
            gx = gx < 0 ? 0 : gx >= kGrid ? kGrid - 1 : gx;
            gz = gz < 0 ? 0 : gz >= kGrid ? kGrid - 1 : gz;
            q->cell = (uint32_t)(gz * kGrid + gx);
            grid[q->cell]++;
        }
        for (int g = 0; g < kGrid * kGrid; g++) h = mix(h, grid[g]);
    }
    for (int i = 0; i < kCount; i++) h = mix(h, fbits(p[i].px) ^ fbits(p[i].py));
    return h;
}

/* ---- hashing: names to ids in an open-addressing table ---- */
static uint32_t fnv1a(const char* s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}
static int make_name(char* buf, uint32_t id) {
    static const char kPrefix[] = "entity/";
    int n = 0;
    for (; kPrefix[n]; n++) buf[n] = kPrefix[n];
    char digits[12];
    int d = 0;
    do digits[d++] = (char)('0' + id % 10); while ((id /= 10) != 0);
    while (d) buf[n++] = digits[--d];
    return n;
}

EXPORT uint32_t bench_hash(uint8_t* scratch, uint32_t reps) {
    enum { kSlots = 1 << 15, kKeys = 20000 };
    uint32_t* keys = (uint32_t*)scratch;  /* hash, value pairs */
    uint32_t found = 0;
    for (uint32_t r = 0; r < reps; r++) {
        for (int k = 0; k < 2 * kSlots; k++) keys[k] = 0;
        char name[32];
        for (uint32_t i = 0; i < kKeys; i++) {
            const uint32_t h = fnv1a(name, make_name(name, i * 7 + r)) | 1;
            uint32_t slot = h & (kSlots - 1);
            while (keys[2 * slot] && keys[2 * slot] != h) slot = (slot + 1) & (kSlots - 1);
            keys[2 * slot] = h, keys[2 * slot + 1] = i;
        }
        for (uint32_t i = 0; i < kKeys; i++) {
            const uint32_t h = fnv1a(name, make_name(name, i * 13 + r)) | 1;
            uint32_t slot = h & (kSlots - 1);
            while (keys[2 * slot] && keys[2 * slot] != h) slot = (slot + 1) & (kSlots - 1);
            if (keys[2 * slot] == h) found = mix(found, keys[2 * slot + 1]);
        }
    }
    return found;
}

/* ---- sorting: render keys through a comparison function ---- */
typedef struct { uint32_t key; float depth; uint32_t id, pad; } draw_item;
typedef int (*compare_fn)(const draw_item*, const draw_item*);

NOINLINE static int by_key_then_depth(const draw_item* a, const draw_item* b) {
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->depth < b->depth ? -1 : a->depth > b->depth ? 1 : (int)a->id - (int)b->id;
}

static void sort_items(draw_item* v, int n, compare_fn cmp) {
    while (n > 16) {  /* quicksort, median of three, smaller half recursively */
        draw_item* a = v;
        draw_item* m = v + n / 2;
        draw_item* z = v + n - 1;
        draw_item t;
        if (cmp(m, a) < 0) t = *m, *m = *a, *a = t;
        if (cmp(z, m) < 0) {
            t = *z, *z = *m, *m = t;
            if (cmp(m, a) < 0) t = *m, *m = *a, *a = t;
        }
        const draw_item pivot = *m;
        int i = 0, j = n - 1;
        for (;;) {
            while (cmp(&v[i], &pivot) < 0) i++;
            while (cmp(&pivot, &v[j]) < 0) j--;
            if (i >= j) break;
            t = v[i], v[i] = v[j], v[j] = t;
            i++, j--;
        }
        if (j + 1 < n - j - 1) {
            sort_items(v, j + 1, cmp);
            v += j + 1, n -= j + 1;
        } else {
            sort_items(v + j + 1, n - j - 1, cmp);
            n = j + 1;
        }
    }
    for (int i = 1; i < n; i++) {  /* insertion sort for short runs */
        const draw_item x = v[i];
        int j = i - 1;
        while (j >= 0 && cmp(&x, &v[j]) < 0) v[j + 1] = v[j], j--;
        v[j + 1] = x;
    }
}

EXPORT uint32_t bench_sort(uint8_t* scratch, uint32_t reps) {
    enum { kItems = 8192 };
    draw_item* v = (draw_item*)scratch;
    uint32_t s = 4242, h = 0;
    for (uint32_t r = 0; r < reps; r++) {
        for (int i = 0; i < kItems; i++) {
            v[i].key = rng_next(&s) >> 22;
            v[i].depth = rng_float(&s) * 1000;
            v[i].id = (uint32_t)i, v[i].pad = 0;
        }
        sort_items(v, kItems, by_key_then_depth);
        for (int i = 0; i < kItems; i += 64) h = mix(h, v[i].id);
    }
    return h;
}

/* ---- entities: virtual updates over objects scattered in memory ----
 * The methods are exported so that the translator, which does not read
 * function tables in data, translates them. */
typedef struct entity entity;
typedef struct {
    void (*update)(entity*, entity* pool, float dt);
    float (*priority)(const entity*);
} entity_vtable;
struct entity {
    const entity_vtable* vt;
    float x, y, z, vx, vy, vz, health, timer;
    uint32_t target, kind, state, pad[3];
};

EXPORT NOINLINE void walker_update(entity* e, entity* pool, float dt) {
    const entity* t = &pool[e->target];
    const float dx = t->x - e->x, dz = t->z - e->z;
    const float d2 = dx * dx + dz * dz;
    if (d2 > 1.0f) {
        const float k = 2.0f * dt / __builtin_sqrtf(d2);
        e->vx = dx * k, e->vz = dz * k;
    } else {
        e->state = (e->state + 1) & 3;
    }
    e->x += e->vx, e->z += e->vz;
}
EXPORT NOINLINE void guard_update(entity* e, entity* pool, float dt) {
    e->timer += dt;
    if (e->timer > 2.0f) {
        e->timer = 0;
        e->target = (e->target * 2654435761u >> 7) & 4095;
    }
    const entity* t = &pool[e->target];
    e->health = clampf(e->health + (t->health - e->health) * 0.01f, 0.0f, 100.0f);
}
EXPORT NOINLINE void prop_update(entity* e, entity* pool, float dt) {
    (void)pool;
    e->vy -= 9.81f * dt;
    e->y += e->vy * dt;
    if (e->y < 0) e->y = 0, e->vy = 0;
}
EXPORT NOINLINE float walker_priority(const entity* e) { return e->health * 0.5f + (float)e->state; }
EXPORT NOINLINE float guard_priority(const entity* e) { return e->health + e->timer; }
EXPORT NOINLINE float prop_priority(const entity* e) { return e->y; }

static const entity_vtable kVtables[3] = {
    {walker_update, walker_priority}, {guard_update, guard_priority}, {prop_update, prop_priority}};

EXPORT uint32_t bench_entities(uint8_t* scratch, uint32_t reps) {
    enum { kEntities = 4096 };
    entity* pool = (entity*)scratch;
    uint32_t* order = (uint32_t*)(pool + kEntities);
    uint32_t s = 99;
    for (int i = 0; i < kEntities; i++) {
        entity* e = &pool[i];
        e->kind = rng_next(&s) % 3;
        e->vt = &kVtables[e->kind];
        e->x = rng_float(&s) * 100, e->y = rng_float(&s) * 5, e->z = rng_float(&s) * 100;
        e->vx = e->vy = e->vz = 0, e->health = rng_float(&s) * 100, e->timer = rng_float(&s);
        e->target = rng_next(&s) & (kEntities - 1), e->state = 0;
        order[i] = (uint32_t)i;
    }
    for (int i = kEntities - 1; i > 0; i--) {  /* visit in a scattered order */
        const uint32_t j = rng_next(&s) % (uint32_t)(i + 1), t = order[i];
        order[i] = order[j], order[j] = t;
    }
    float acc = 0;
    for (uint32_t r = 0; r < reps; r++)
        for (int i = 0; i < kEntities; i++) {
            entity* e = &pool[order[i]];
            e->vt->update(e, pool, 1.0f / 30.0f);
            acc += e->vt->priority(e) * 0.0001f;
        }
    return fbits(acc);
}

/* ---- tree: inserts and lookups in a binary search tree (pointer chasing) ---- */
typedef struct { uint32_t key, left, right, value; } tree_node;

EXPORT uint32_t bench_tree(uint8_t* scratch, uint32_t reps) {
    enum { kNodes = 1 << 16 };
    tree_node* n = (tree_node*)scratch;  /* node 0 is the null link */
    uint32_t h = 0;
    for (uint32_t r = 0; r < reps; r++) {
        uint32_t s = 31337 + r, count = 1, root = 0;
        for (uint32_t i = 0; i < kNodes - 1; i++) {
            const uint32_t key = rng_next(&s);
            uint32_t* link = &root;
            while (*link) link = key < n[*link].key ? &n[*link].left : &n[*link].right;
            n[count].key = key, n[count].left = n[count].right = 0, n[count].value = i;
            *link = count++;
        }
        s = 31337 + r;
        for (uint32_t i = 0; i < kNodes - 1; i++) {
            uint32_t key = rng_next(&s);
            if (i & 1) key ^= 0x5555;  /* half of them miss */
            uint32_t at = root;
            while (at && n[at].key != key) at = key < n[at].key ? n[at].left : n[at].right;
            h = mix(h, at ? n[at].value : 0xffffffffu);
        }
    }
    return h;
}

/* ---- text: tokenising a generated configuration file ---- */
EXPORT uint32_t bench_text(uint8_t* scratch, uint32_t reps) {
    enum { kSize = 64 * 1024 };
    char* text = (char*)scratch;
    uint32_t s = 2024;
    int len = 0;
    while (len < kSize - 64) {  /* lines like "Name_123 = 0.75 ; comment" */
        static const char* kWords[] = {"Quality", "Shadow", "Texture", "Distance", "Crowd", "Water"};
        const char* w = kWords[rng_next(&s) % 6];
        while (*w) text[len++] = *w++;
        text[len++] = '_';
        len += make_name(text + len, rng_next(&s) % 1000) - 7;  /* digits only */
        text[len++] = ' ', text[len++] = '=', text[len++] = ' ';
        const uint32_t v = rng_next(&s) % 10000;
        text[len++] = (char)('0' + v / 1000), text[len++] = '.';
        text[len++] = (char)('0' + v / 100 % 10), text[len++] = (char)('0' + v / 10 % 10);
        if (rng_next(&s) & 1) {
            static const char kComment[] = " ; tuned";
            for (int k = 0; kComment[k]; k++) text[len++] = kComment[k];
        }
        text[len++] = '\n';
    }
    uint32_t h = 0;
    for (uint32_t r = 0; r < reps; r++) {
        float total = 0;
        int lines = 0;
        for (int i = 0; i < len;) {
            int key_start = i;
            while (i < len && text[i] != ' ' && text[i] != '\n') i++;
            const uint32_t key = fnv1a(text + key_start, i - key_start);
            while (i < len && (text[i] == ' ' || text[i] == '=')) i++;
            int whole = 0, frac = 0, scale = 1;
            while (i < len && text[i] >= '0' && text[i] <= '9') whole = whole * 10 + (text[i++] - '0');
            if (i < len && text[i] == '.')
                for (i++; i < len && text[i] >= '0' && text[i] <= '9'; i++) frac = frac * 10 + (text[i] - '0'), scale *= 10;
            while (i < len && text[i] != '\n') i++;  /* skip comments */
            i++;
            total += (float)whole + (float)frac / (float)scale;
            h = mix(h, key + (uint32_t)r);
            lines++;
        }
        h = mix(h, fbits(total) ^ (uint32_t)lines);
    }
    return h;
}

/* ---- double precision: a few iterations of a small orbit integrator ---- */
EXPORT uint32_t bench_double(uint8_t* scratch, uint32_t reps) {
    enum { kBodies = 64 };
    double* b = (double*)scratch;  /* x, y, vx, vy, mass */
    uint32_t s = 5;
    for (int i = 0; i < kBodies; i++) {
        b[i * 5 + 0] = rng_float(&s) * 100.0, b[i * 5 + 1] = rng_float(&s) * 100.0;
        b[i * 5 + 2] = 0, b[i * 5 + 3] = 0, b[i * 5 + 4] = 1.0 + rng_float(&s);
    }
    for (uint32_t r = 0; r < reps; r++)
        for (int i = 0; i < kBodies; i++) {
            double ax = 0, ay = 0;
            for (int j = 0; j < kBodies; j++) {
                if (j == i) continue;
                const double dx = b[j * 5] - b[i * 5], dy = b[j * 5 + 1] - b[i * 5 + 1];
                const double d2 = dx * dx + dy * dy + 0.01;
                const double inv = 1.0 / (d2 * __builtin_sqrt(d2));
                ax += dx * inv * b[j * 5 + 4], ay += dy * inv * b[j * 5 + 4];
            }
            b[i * 5 + 2] += ax * 0.001, b[i * 5 + 3] += ay * 0.001;
            b[i * 5 + 0] += b[i * 5 + 2] * 0.001, b[i * 5 + 1] += b[i * 5 + 3] * 0.001;
        }
    uint32_t h = 0;
    for (int i = 0; i < kBodies * 5; i++) h = mix(h, fbits((float)b[i]));
    return h;
}
