/* setup_inv.c -- the machine inventory; see setup_inv.h. */
#include "setup_inv.h"

unsigned si_len(const char *s)
{
    unsigned n = 0;
    while (s && s[n])
        n++;
    return n;
}

unsigned si_copy(char *d, const char *s, unsigned cap)
{
    unsigned n = 0;
    while (s && s[n] && n + 1 < cap)
        d[n] = s[n], n++;
    d[n] = 0;
    return n;
}

unsigned si_cat(char *d, unsigned at, const char *s, unsigned cap)
{
    while (s && *s && at + 1 < cap)
        d[at++] = *s++;
    d[at < cap ? at : cap - 1] = 0;
    return at;
}

unsigned si_num(char *d, unsigned at, uint32_t v, unsigned cap)
{
    char t[12];
    unsigned n = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < sizeof t);
    while (n && at + 1 < cap)
        d[at++] = t[--n];
    d[at < cap ? at : cap - 1] = 0;
    return at;
}

static void zero(void *p, unsigned n)
{
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

void si_init(si_inv *v) { zero(v, sizeof *v); }

/* ---- the host description ---- */

typedef struct {
    const char *k, *v;
    unsigned kn, vn;
} kv;

#define KV_MAX 12

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static int eq(const char *s, unsigned n, const char *w)
{
    unsigned i = 0;
    for (; i < n && w[i]; i++)
        if (s[i] != w[i])
            return 0;
    return i == n && !w[i];
}

/* One line: its kind word and up to KV_MAX key=value pairs; 0 if malformed. */
static int split(const char *s, unsigned n, const char **kind, unsigned *kindn, kv *out, unsigned *nkv,
                 unsigned *bad)
{
    unsigned i = 0, m = 0;
    while (i < n && is_space(s[i]))
        i++;
    *kind = s + i;
    while (i < n && !is_space(s[i]))
        i++;
    *kindn = (unsigned)(s + i - *kind);
    for (;;) {
        while (i < n && is_space(s[i]))
            i++;
        if (i >= n)
            break;
        unsigned k0 = i;
        while (i < n && s[i] != '=' && !is_space(s[i]))
            i++;
        if (i >= n || s[i] != '=') { /* a word with no value: skipped */
            (*bad)++;
            while (i < n && !is_space(s[i]))
                i++;
            continue;
        }
        unsigned kn = i - k0, v0, vn;
        i++;
        if (i < n && s[i] == '"') {
            v0 = ++i;
            while (i < n && s[i] != '"')
                i++;
            if (i >= n)
                return 0; /* an unclosed quote spoils the line */
            vn = i - v0;
            i++;
        } else {
            v0 = i;
            while (i < n && !is_space(s[i]))
                i++;
            vn = i - v0;
        }
        if (m < KV_MAX)
            out[m++] = (kv){s + k0, s + v0, kn, vn};
        else
            (*bad)++;
    }
    *nkv = m;
    return *kindn > 0;
}

static void put(char *d, unsigned cap, const kv *p)
{
    unsigned n = p->vn < cap - 1 ? p->vn : cap - 1;
    for (unsigned i = 0; i < n; i++)
        d[i] = p->v[i] >= ' ' && p->v[i] < 127 ? p->v[i] : '?';
    d[n] = 0;
}

static uint32_t num(const kv *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < p->vn && p->v[i] >= '0' && p->v[i] <= '9'; i++) {
        v = v * 10 + (uint32_t)(p->v[i] - '0');
        if (v > 0xffffffffu)
            return 0xffffffffu;
    }
    return (uint32_t)v;
}

static uint16_t hex16(const kv *p)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < p->vn && i < 4; i++) {
        char c = p->v[i];
        unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0')
                     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                            : 16u;
        if (d > 15)
            break;
        v = v << 4 | d;
    }
    return (uint16_t)v;
}

static uint16_t u16(uint32_t v) { return v > 0xffffu ? 0xffffu : (uint16_t)v; }

static int item(si_inv *v, const char *kind, unsigned kn, const kv *p, unsigned m)
{
#define KEY(w) eq(p[i].k, p[i].kn, w)
    if (eq(kind, kn, "gpu")) {
        if (v->ngpu >= SI_GPUS)
            return 0;
        si_gpu *g = &v->gpu[v->ngpu];
        zero(g, sizeof *g);
        g->from = SI_FROM_HOST;
        g->kind = SI_GPU_DISCRETE;
        for (unsigned i = 0; i < m; i++) {
            if (KEY("vendor"))
                g->vendor = hex16(&p[i]);
            else if (KEY("device"))
                g->device = hex16(&p[i]);
            else if (KEY("name"))
                put(g->name, sizeof g->name, &p[i]);
            else if (KEY("vram_mib"))
                g->vram_mib = num(&p[i]);
            else if (KEY("driver"))
                put(g->driver, sizeof g->driver, &p[i]);
            else if (KEY("kind"))
                g->kind = eq(p[i].v, p[i].vn, "integrated") ? SI_GPU_INTEGRATED
                          : eq(p[i].v, p[i].vn, "unified")  ? SI_GPU_UNIFIED
                          : eq(p[i].v, p[i].vn, "virtual")  ? SI_GPU_VIRTUAL
                                                            : SI_GPU_DISCRETE;
            else
                v->skipped++;
        }
        v->ngpu++;
    } else if (eq(kind, kn, "display")) {
        if (v->ndisplay >= SI_DISPLAYS)
            return 0;
        si_display *d = &v->display[v->ndisplay];
        zero(d, sizeof *d);
        d->from = SI_FROM_HOST;
        for (unsigned i = 0; i < m; i++) {
            if (KEY("name"))
                put(d->name, sizeof d->name, &p[i]);
            else if (KEY("w"))
                d->w = u16(num(&p[i]));
            else if (KEY("h"))
                d->h = u16(num(&p[i]));
            else if (KEY("hz"))
                d->hz = u16(num(&p[i]));
            else if (KEY("max_w"))
                d->max_w = u16(num(&p[i]));
            else if (KEY("max_h"))
                d->max_h = u16(num(&p[i]));
            else if (KEY("max_hz"))
                d->max_hz = u16(num(&p[i]));
            else if (KEY("vrr"))
                d->vrr = num(&p[i]) != 0;
            else
                v->skipped++;
        }
        v->ndisplay++;
    } else if (eq(kind, kn, "cpu")) {
        zero(&v->cpu, sizeof v->cpu);
        v->cpu.from = SI_FROM_HOST;
        for (unsigned i = 0; i < m; i++) {
            if (KEY("name"))
                put(v->cpu.name, sizeof v->cpu.name, &p[i]);
            else if (KEY("fast"))
                v->cpu.fast = u16(num(&p[i]));
            else if (KEY("efficient"))
                v->cpu.efficient = u16(num(&p[i]));
            else if (KEY("threads"))
                v->cpu.threads = u16(num(&p[i]));
            else
                v->skipped++;
        }
    } else if (eq(kind, kn, "ram")) {
        for (unsigned i = 0; i < m; i++)
            if (KEY("mib"))
                v->ram_mib = num(&p[i]);
            else
                v->skipped++;
    } else if (eq(kind, kn, "disk")) {
        if (v->ndisk >= SI_DISKS)
            return 0;
        si_disk *d = &v->disk[v->ndisk];
        zero(d, sizeof *d);
        d->from = SI_FROM_HOST;
        for (unsigned i = 0; i < m; i++) {
            if (KEY("name"))
                put(d->name, sizeof d->name, &p[i]);
            else if (KEY("gib"))
                d->gib = num(&p[i]);
            else if (KEY("kind"))
                d->kind = eq(p[i].v, p[i].vn, "nvme")  ? SI_DISK_NVME
                          : eq(p[i].v, p[i].vn, "ssd") ? SI_DISK_SSD
                          : eq(p[i].v, p[i].vn, "hdd") ? SI_DISK_HDD
                                                       : SI_DISK_UNKNOWN;
            else
                v->skipped++;
        }
        v->ndisk++;
    } else if (eq(kind, kn, "usb")) {
        if (v->nusb >= SI_USB)
            return 0;
        for (unsigned i = 0; i < m; i++)
            if (KEY("name"))
                put(v->usb[v->nusb], SI_NAME, &p[i]);
            else
                v->skipped++;
        v->nusb++;
    } else if (eq(kind, kn, "acpi")) {
        for (unsigned i = 0; i < m; i++)
            if (KEY("tables"))
                put(v->acpi, sizeof v->acpi, &p[i]);
            else
                v->skipped++;
    } else if (eq(kind, kn, "gateway")) {
        for (unsigned i = 0; i < m; i++)
            if (KEY("addr"))
                put(v->gateway, sizeof v->gateway, &p[i]);
            else
                v->skipped++;
    } else if (eq(kind, kn, "peer")) {
        if (v->npeer >= SI_PEERS)
            return 0;
        si_peer *q = &v->peer[v->npeer];
        zero(q, sizeof *q);
        q->kind = SI_PEER_OTHER;
        for (unsigned i = 0; i < m; i++) {
            if (KEY("name"))
                put(q->name, sizeof q->name, &p[i]);
            else if (KEY("addr"))
                put(q->addr, sizeof q->addr, &p[i]);
            else if (KEY("model"))
                put(q->model, sizeof q->model, &p[i]);
            else if (KEY("url"))
                put(q->url, sizeof q->url, &p[i]);
            else if (KEY("params_b10"))
                q->params_b10 = u16(num(&p[i]));
            else if (KEY("kind"))
                q->kind = eq(p[i].v, p[i].vn, "aienos")    ? SI_PEER_AIENOS
                          : eq(p[i].v, p[i].vn, "printer") ? SI_PEER_PRINTER
                          : eq(p[i].v, p[i].vn, "nas")     ? SI_PEER_NAS
                                                           : SI_PEER_OTHER;
            else
                v->skipped++;
        }
        v->npeer++;
    } else if (eq(kind, kn, "model") || eq(kind, kn, "server")) {
        if (v->nmodel >= SI_MODELS)
            return 0;
        si_model *o = &v->model[v->nmodel];
        zero(o, sizeof *o);
        o->running = eq(kind, kn, "server");
        o->src = o->running ? SI_SRC_SERVER : SI_SRC_LLAMACPP;
        for (unsigned i = 0; i < m; i++) {
            if (KEY("name"))
                put(o->name, sizeof o->name, &p[i]);
            else if (KEY("path") || KEY("url"))
                put(o->where, sizeof o->where, &p[i]);
            else if (KEY("params_b10"))
                o->params_b10 = u16(num(&p[i]));
            else if (KEY("bits10"))
                o->bits10 = u16(num(&p[i]));
            else if (KEY("mib"))
                o->mib = num(&p[i]);
            else if (KEY("source"))
                o->src = eq(p[i].v, p[i].vn, "ollama")     ? SI_SRC_OLLAMA
                         : eq(p[i].v, p[i].vn, "lmstudio") ? SI_SRC_LMSTUDIO
                         : eq(p[i].v, p[i].vn, "llamacpp") ? SI_SRC_LLAMACPP
                         : eq(p[i].v, p[i].vn, "pair")     ? SI_SRC_PAIR
                                                           : o->src;
            else
                v->skipped++;
        }
        if (!o->name[0])
            return 0;
        v->nmodel++;
    } else if (eq(kind, kn, "machine")) {
        for (unsigned i = 0; i < m; i++)
            if (KEY("name"))
                put(v->machine, sizeof v->machine, &p[i]);
            else if (!KEY("from"))
                v->skipped++;
    } else {
        return 0;
    }
#undef KEY
    return 1;
}

unsigned si_parse(si_inv *v, const char *text, unsigned n)
{
    unsigned taken = 0, i = 0;
    while (i < n) {
        unsigned s = i;
        while (i < n && text[i] != '\n')
            i++;
        unsigned len = i - s;
        i++;
        const char *line = text + s;
        unsigned j = 0;
        while (j < len && is_space(line[j]))
            j++;
        if (j == len || line[j] == '#')
            continue;
        const char *kind;
        unsigned kn, m = 0;
        kv p[KV_MAX];
        if (!split(line, len, &kind, &kn, p, &m, &v->skipped) || !item(v, kind, kn, p, m)) {
            v->skipped++;
            continue;
        }
        taken++;
    }
    if (taken)
        v->host = 1;
    return taken;
}

/* ---- what the kernel found ---- */

const char *si_vendor(uint16_t vendor)
{
    switch (vendor) {
    case 0x10de:
        return "NVIDIA";
    case 0x1002:
        return "AMD";
    case 0x8086:
        return "Intel";
    case 0x1af4:
        return "virtio";
    case 0x1234:
        return "QEMU";
    case 0x15ad:
        return "VMware";
    case 0x106b:
        return "Apple";
    default:
        return "a";
    }
}

void si_add_gpu(si_inv *v, uint16_t vendor, uint16_t device, uint32_t pci_class)
{
    if (v->ngpu >= SI_GPUS)
        return;
    si_gpu *g = &v->gpu[v->ngpu++];
    zero(g, sizeof *g);
    g->vendor = vendor, g->device = device, g->from = SI_FROM_HERE;
    g->kind = vendor == 0x1234 || vendor == 0x1af4 || vendor == 0x15ad ? SI_GPU_VIRTUAL
              : vendor == 0x8086                                         ? SI_GPU_INTEGRATED
                                                                         : SI_GPU_DISCRETE;
    unsigned n = si_copy(g->name, si_vendor(vendor), sizeof g->name);
    n = si_cat(g->name, n, g->kind == SI_GPU_VIRTUAL ? " virtual display" : " graphics", sizeof g->name);
    (void)pci_class;
    (void)n;
}

void si_add_display(si_inv *v, uint16_t w, uint16_t h)
{
    if (v->ndisplay >= SI_DISPLAYS)
        return;
    si_display *d = &v->display[v->ndisplay++];
    zero(d, sizeof *d);
    si_copy(d->name, "this screen", sizeof d->name);
    d->w = d->max_w = w, d->h = d->max_h = h;
}

void si_add_cpus(si_inv *v, unsigned count, const char *name)
{
    if (v->cpu.from == SI_FROM_HOST && v->cpu.fast)
        return; /* the host's own processor says more */
    zero(&v->cpu, sizeof v->cpu);
    v->cpu.fast = u16(count), v->cpu.threads = u16(count);
    si_copy(v->cpu.name, name, sizeof v->cpu.name);
}

void si_add_disk(si_inv *v, const char *name, uint64_t bytes, uint8_t kind)
{
    if (v->ndisk >= SI_DISKS)
        return;
    si_disk *d = &v->disk[v->ndisk++];
    zero(d, sizeof *d);
    si_copy(d->name, name, sizeof d->name);
    d->gib = (uint32_t)(bytes >> 30);
    d->kind = kind;
}

void si_add_ram(si_inv *v, uint64_t bytes)
{
    if (!v->ram_mib)
        v->ram_mib = (uint32_t)(bytes >> 20);
}

int si_best_gpu(const si_inv *v)
{
    int best = -1;
    for (unsigned i = 0; i < v->ngpu; i++)
        if (v->gpu[i].kind != SI_GPU_VIRTUAL && (best < 0 || v->gpu[i].vram_mib > v->gpu[best].vram_mib))
            best = (int)i;
    return best;
}

/* ---- in plain words ---- */

/* "24 GB" from MiB: whole gigabytes (1024 MiB each), rounded to nearest */
static unsigned gb(char *d, unsigned at, uint32_t mib, unsigned cap)
{
    if (mib && mib < 1024) {
        at = si_num(d, at, mib, cap);
        return si_cat(d, at, " MB", cap);
    }
    at = si_num(d, at, (mib + 512) / 1024, cap);
    return si_cat(d, at, " GB", cap);
}

static unsigned disk_size(char *d, unsigned at, uint32_t gib, unsigned cap)
{
    if (gib >= 1000) { /* 1.9 TB style: one decimal */
        uint32_t t10 = (gib * 10 + 500) / 1000;
        at = si_num(d, at, t10 / 10, cap);
        if (t10 % 10) {
            at = si_cat(d, at, ".", cap);
            at = si_num(d, at, t10 % 10, cap);
        }
        return si_cat(d, at, " TB", cap);
    }
    at = si_num(d, at, gib, cap);
    return si_cat(d, at, " GB", cap);
}

unsigned si_summary(const si_inv *v, char (*out)[96], unsigned max)
{
    unsigned n = 0;
    const unsigned cap = 96;
    /* in the order of FIRST_RUN_UX screen 5: screens, graphics, processor, memory, storage, nearby */
    for (unsigned i = 0; i < v->ndisplay && n < max && i < 2; i++) {
        const si_display *s = &v->display[i];
        char *d = out[n++];
        unsigned at = si_copy(d, "Screen: ", cap);
        at = si_cat(d, at, s->name, cap);
        at = si_cat(d, at, ", ", cap);
        at = si_num(d, at, s->w, cap);
        at = si_cat(d, at, " by ", cap);
        at = si_num(d, at, s->h, cap);
        if (s->hz) {
            at = si_cat(d, at, ", ", cap);
            at = si_num(d, at, s->hz, cap);
            at = si_cat(d, at, " times a second", cap);
        }
        if (s->max_hz > s->hz && s->hz) {
            at = si_cat(d, at, " (it can do ", cap);
            at = si_num(d, at, s->max_hz, cap);
            at = si_cat(d, at, ")", cap);
        }
    }
    /* graphics: the best card, then how many more */
    int g = si_best_gpu(v);
    if (n < max) {
        char *d = out[n++];
        unsigned at = si_copy(d, "Graphics: ", cap);
        if (g < 0) {
            at = si_cat(d, at, v->ngpu ? "only a simple virtual display" : "none found", cap);
        } else {
            at = si_cat(d, at, v->gpu[g].name, cap);
            if (v->gpu[g].vram_mib) {
                at = si_cat(d, at, ", ", cap);
                at = gb(d, at, v->gpu[g].vram_mib, cap);
                at = si_cat(d, at, v->gpu[g].kind == SI_GPU_UNIFIED ? " of shared memory" : " of its own memory",
                            cap);
            }
            if (v->ngpu > 1) {
                at = si_cat(d, at, ", and ", cap);
                at = si_num(d, at, v->ngpu - 1, cap);
                at = si_cat(d, at, " more", cap);
            }
        }
    }
    if (n < max && v->cpu.fast) {
        char *d = out[n++];
        unsigned at = si_copy(d, "Processor: ", cap);
        unsigned cores = (unsigned)v->cpu.fast + v->cpu.efficient;
        if (v->cpu.name[0] && v->cpu.from == SI_FROM_HOST) {
            at = si_cat(d, at, v->cpu.name, cap);
            at = si_cat(d, at, ", ", cap);
        }
        at = si_num(d, at, cores, cap);
        at = si_cat(d, at, cores == 1 ? " core" : " cores", cap);
        if (v->cpu.efficient) {
            at = si_cat(d, at, " (", cap);
            at = si_num(d, at, v->cpu.fast, cap);
            at = si_cat(d, at, " fast, ", cap);
            at = si_num(d, at, v->cpu.efficient, cap);
            at = si_cat(d, at, " efficient)", cap);
        }
    }
    if (n < max && v->ram_mib) {
        char *d = out[n++];
        unsigned at = si_copy(d, "Memory: ", cap);
        gb(d, at, v->ram_mib, cap);
    }
    if (n < max && v->ndisk) {
        char *d = out[n++];
        unsigned at = si_copy(d, "Storage: ", cap);
        for (unsigned i = 0; i < v->ndisk && i < 2; i++) {
            if (i)
                at = si_cat(d, at, "; ", cap);
            at = disk_size(d, at, v->disk[i].gib, cap);
            at = si_cat(d, at, v->disk[i].kind == SI_DISK_NVME  ? " fast drive"
                               : v->disk[i].kind == SI_DISK_SSD ? " solid-state drive"
                               : v->disk[i].kind == SI_DISK_HDD ? " hard disk"
                                                                : " disk",
                        cap);
        }
        if (v->ndisk > 2) {
            at = si_cat(d, at, " and ", cap);
            at = si_num(d, at, v->ndisk - 2, cap);
            at = si_cat(d, at, " more", cap);
        }
    }
    if (n < max && (v->npeer || v->gateway[0])) {
        char *d = out[n++];
        unsigned at = si_copy(d, "Nearby: ", cap), aienos = 0;
        for (unsigned i = 0; i < v->npeer; i++)
            aienos += v->peer[i].kind == SI_PEER_AIENOS;
        if (aienos) {
            at = si_num(d, at, aienos, cap);
            at = si_cat(d, at, aienos == 1 ? " other AienOS computer (" : " other AienOS computers (", cap);
            unsigned shown = 0;
            for (unsigned i = 0; i < v->npeer && shown < 2; i++)
                if (v->peer[i].kind == SI_PEER_AIENOS) {
                    if (shown++)
                        at = si_cat(d, at, ", ", cap);
                    at = si_cat(d, at, v->peer[i].name, cap);
                }
            at = si_cat(d, at, ")", cap);
        }
        if (v->npeer > aienos) {
            at = si_cat(d, at, aienos ? ", " : "", cap);
            at = si_num(d, at, v->npeer - aienos, cap);
            at = si_cat(d, at, v->npeer - aienos == 1 ? " other device" : " other devices", cap);
        }
        if (v->gateway[0])
            at = si_cat(d, at, aienos || v->npeer ? ", and your router" : "your router", cap);
    }
    if (n < max && v->nmodel) {
        char *d = out[n++];
        unsigned at = si_copy(d, "Models already here: ", cap);
        for (unsigned i = 0; i < v->nmodel && i < 2; i++) {
            if (i)
                at = si_cat(d, at, ", ", cap);
            at = si_cat(d, at, v->model[i].name, cap);
        }
        if (v->nmodel > 2) {
            at = si_cat(d, at, " and ", cap);
            at = si_num(d, at, v->nmodel - 2, cap);
            at = si_cat(d, at, " more", cap);
        }
    }
    if (n < max && (v->nusb || v->npci)) {
        char *d = out[n++];
        unsigned at = si_copy(d, "Also: ", cap);
        if (v->nusb) {
            at = si_num(d, at, v->nusb, cap);
            at = si_cat(d, at, v->nusb == 1 ? " USB device" : " USB devices", cap);
        }
        if (v->npci) {
            at = si_cat(d, at, v->nusb ? ", " : "", cap);
            at = si_num(d, at, v->npci, cap);
            at = si_cat(d, at, " parts on the main board", cap);
        }
    }
    return n;
}
