/* test_pipe.c -- host tests for core/pipe.c (aienos-mv#428): the fixture corpus in tests/fixtures/pipe/ (format in its
 * README.md), then producer / consumer transfers through the scheduler (core/pipe_drive.h), revocation mid-stream,
 * epoch staleness and the versioned op table, then pthreads (concurrent read/write, close while parked, shared-pool
 * create/close). Every case ends with both capability tables and the pipe pool empty. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ck_test.h"
#include "pipe.h"
#include "pipe_drive.h"

#ifndef PIPE_FIXTURE_DIR
#define PIPE_FIXTURE_DIR "native/kernel/tests/fixtures/pipe"
#endif

static struct ck_pipe_pool pool;
static struct ck_cap_table ta, tb;

static int expect(int ok, const char *file, int line, const char *what)
{
    ck_t_run++;
    if (!ok) {
        ck_t_fail++;
        printf("  FAIL %s:%d: %s\n", file, line, what);
    }
    return ok;
}

/* ---- fixture runner ---- */
#define TOK_MAX 24
#define BYTES_MAX 256
struct tok {
    char s[64];
    int quoted;
    uint8_t b[BYTES_MAX];
    size_t n;
};
struct name {
    char s[16];
    struct ck_cap_table *t;
    struct ck_handle h;
};
static struct name names[32];
static unsigned nnames;
static const char *cur_file;
static int cur_line;

/* the wait hook's scripted peer: first wait runs the action, every later wait gives up (a stalled peer) */
static struct tok *act;
static int nact, wait_calls;
static unsigned act_err;

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int tokenize(const char *line, struct tok *t, int max)
{
    int n = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p || *p == '#')
            break;
        if (n == max)
            return -1;
        struct tok *k = &t[n++];
        memset(k, 0, sizeof *k);
        if (*p == '"') {
            k->quoted = 1;
            p++;
            while (*p && *p != '"') {
                int c = (unsigned char)*p++;
                if (c == '\\') {
                    if (*p == 'x' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                        c = hexval(p[1]) * 16 + hexval(p[2]);
                        p += 3;
                    } else if (*p == '"' || *p == '\\') {
                        c = (unsigned char)*p++;
                    } else {
                        return -1;
                    }
                }
                if (k->n == BYTES_MAX)
                    return -1;
                k->b[k->n++] = (uint8_t)c;
            }
            if (*p != '"')
                return -1;
            p++;
            strcpy(k->s, "\"\"");
        } else {
            size_t l = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '"') {
                if (l + 1 >= sizeof k->s)
                    return -1;
                k->s[l++] = *p++;
            }
        }
    }
    return n;
}

static struct ck_cap_table *table_of(const char *s)
{
    return !strcmp(s, "A") ? &ta : !strcmp(s, "B") ? &tb : 0;
}

static struct name *find(const char *s)
{
    for (unsigned i = 0; i < nnames; i++)
        if (!strcmp(names[i].s, s))
            return &names[i];
    return 0;
}

static void bind(const char *s, struct ck_cap_table *t, struct ck_handle h)
{
    struct name *n = find(s);
    if (!n && nnames < 32)
        n = &names[nnames++];
    if (!n)
        return;
    snprintf(n->s, sizeof n->s, "%s", s);
    n->t = t;
    n->h = h;
}

static int parse_err(const char *s)
{
    for (unsigned i = 0; i < OSH_E_COUNT; i++)
        if (!strcmp(s, osh_err_label(i)))
            return (int)i;
    return -1;
}

static int parse_rights(const char *s, unsigned *r)
{
    *r = 0;
    for (; *s; s++) {
        switch (*s) {
        case 'r': *r |= CK_R_READ; break;
        case 'w': *r |= CK_R_WRITE; break;
        case 'm': *r |= CK_R_MAP; break;
        case 'g': *r |= CK_R_GRANT; break;
        case 'd': *r |= CK_R_DERIVE; break;
        case 'v': *r |= CK_R_REVOKE; break;
        default: return -1;
        }
    }
    return 0;
}

static uint8_t big[70000], rbig[70000];

/* Runs one data or control command (also used for the wait hook's action). *n and the read bytes are returned. */
static int run_cmd(struct tok *c, int nc, unsigned *err, size_t *n, const uint8_t **data, int *handled_bind)
{
    *n = 0;
    *data = 0;
    *handled_bind = 0;
    struct name *h = nc > 1 ? find(c[1].s) : 0;
    if (!strcmp(c[0].s, "write") && nc == 3 && h && c[2].quoted) {
        *err = ck_stream_write(&pool, h->t, h->h, c[2].b, c[2].n, 0, n);
    } else if (!strcmp(c[0].s, "fill") && nc == 4 && h) {
        size_t cnt = strtoul(c[2].s, 0, 10);
        if (cnt > sizeof big)
            return -1;
        memset(big, (int)strtoul(c[3].s, 0, 16), cnt);
        *err = ck_stream_write(&pool, h->t, h->h, big, cnt, 0, n);
    } else if (!strcmp(c[0].s, "read") && nc == 3 && h) {
        size_t max = strtoul(c[2].s, 0, 10);
        if (max > sizeof rbig)
            return -1;
        *err = ck_stream_read(&pool, h->t, h->h, rbig, max, 0, n);
        *data = rbig;
    } else if (!strcmp(c[0].s, "close") && nc == 2 && h) {
        *err = ck_stream_close(&pool, h->t, h->h);
    } else if (!strcmp(c[0].s, "revoke") && nc == 2 && h) {
        *err = ck_pipe_revoke(&pool, h->t, h->h);
    } else if (!strcmp(c[0].s, "cancel") && nc == 2 && h) {
        *err = ck_pipe_cancel(&pool, h->t, h->h);
    } else {
        return -1;
    }
    return 0;
}

static unsigned vec_wait(void *ctx, unsigned pipe, unsigned end)
{
    (void)ctx;
    (void)pipe;
    (void)end;
    if (wait_calls++ > 0 || !act)
        return OSH_E_INTERRUPTED;
    if (nact == 1 && !strcmp(act[0].s, "none"))
        return OSH_E_OK;
    size_t n;
    const uint8_t *d;
    int hb;
    if (run_cmd(act, nact, &act_err, &n, &d, &hb) != 0)
        act_err = 0xff;
    return OSH_E_OK;
}
static void vec_wake(void *ctx, unsigned pipe, unsigned end)
{
    (void)ctx;
    (void)pipe;
    (void)end;
}
static const struct ck_pipe_hooks vec_hooks = { 0, vec_wait, vec_wake, 0 };

static uint64_t stat_of(const char *k, int *ok)
{
    const struct ck_pipe_stats *s = &pool.total;
    *ok = 1;
#define S(f) if (!strcmp(k, #f)) return s->f;
    S(bytes_written) S(bytes_read) S(writes) S(reads) S(max_resident) S(would_block) S(waits) S(wakeups)
    S(cancellations) S(revocations) S(broken_pipe) S(eof) S(refused)
#undef S
    if (!strcmp(k, "pipes"))
        return pool.live;
    *ok = 0;
    return 0;
}

static int all_empty(void)
{
    return pd_table_empty(&ta) && pd_table_empty(&tb) && ck_pipe_pool_empty(&pool);
}

static void run_line(struct tok *t, int nt)
{
    char what[160];
    if (!strcmp(t[0].s, "case") || !strcmp(t[0].s, "kind"))
        return;
    if (!strcmp(t[0].s, "check")) {
        for (int i = 1; i < nt; i++) {
            if (!strcmp(t[i].s, "empty")) {
                expect(all_empty(), cur_file, cur_line, "check empty: tables and pool hold nothing");
                continue;
            }
            char key[64];
            snprintf(key, sizeof key, "%s", t[i].s);
            char *eq = strchr(key, '=');
            int ok = 0;
            if (eq) {
                *eq = 0;
                uint64_t want = strtoull(eq + 1, 0, 10), got = stat_of(key, &ok);
                snprintf(what, sizeof what, "check %s: want %llu got %llu", key, (unsigned long long)want,
                         (unsigned long long)got);
                ok = ok && got == want;
            } else {
                snprintf(what, sizeof what, "check: bad item %s", t[i].s);
            }
            expect(ok, cur_file, cur_line, what);
        }
        return;
    }
    int arrow = -1;
    for (int i = 0; i < nt; i++)
        if (!t[i].quoted && !strcmp(t[i].s, "=>"))
            arrow = i;
    if (!expect(arrow > 0 && arrow + 1 < nt, cur_file, cur_line, "line has '<command> => <ERR>'"))
        return;
    int want = parse_err(t[arrow + 1].s);
    if (!expect(want >= 0, cur_file, cur_line, "known error name"))
        return;
    long want_n = -1;
    struct tok *want_data = 0;
    for (int i = arrow + 2; i < nt; i++) {
        if (!strncmp(t[i].s, "n=", 2))
            want_n = strtol(t[i].s + 2, 0, 10);
        else if (!strcmp(t[i].s, "data=") && i + 1 < nt && t[i + 1].quoted)
            want_data = &t[++i];
        else
            expect(0, cur_file, cur_line, "unknown expectation item");
    }
    struct tok *c = t;
    int nc = arrow;
    unsigned err = 0xff;
    size_t n = 0;
    const uint8_t *data = 0;
    struct name *h = nc > 1 ? find(c[1].s) : 0;
    act = 0;
    nact = 0;
    wait_calls = 0;
    act_err = 0;
    if (!strcmp(c[0].s, "create") && nc == 4 && table_of(c[3].s)) {
        struct ck_handle r, w;
        err = ck_pipe_create(&pool, table_of(c[3].s), &r, &w);
        if (err == OSH_E_OK) {
            bind(c[1].s, table_of(c[3].s), r);
            bind(c[2].s, table_of(c[3].s), w);
        }
    } else if (!strcmp(c[0].s, "dup") && nc == 5 && h && table_of(c[3].s)) {
        unsigned rights;
        struct ck_handle o;
        if (!expect(parse_rights(c[4].s, &rights) == 0, cur_file, cur_line, "rights letters"))
            return;
        err = ck_stream_dup(&pool, h->t, h->h, table_of(c[3].s), rights, &o);
        if (err == OSH_E_OK)
            bind(c[2].s, table_of(c[3].s), o);
    } else if (!strcmp(c[0].s, "release") && nc == 2 && table_of(c[1].s)) {
        n = ck_pipe_release_table(&pool, table_of(c[1].s));
        err = OSH_E_OK;
    } else if ((!strcmp(c[0].s, "bread") || !strcmp(c[0].s, "bwrite")) && nc >= 4 && h) {
        act = &c[3];
        nact = nc - 3;
        if (c[0].s[1] == 'r') {
            size_t max = strtoul(c[2].s, 0, 10);
            if (!expect(max <= sizeof rbig, cur_file, cur_line, "read size"))
                return;
            err = ck_stream_read(&pool, h->t, h->h, rbig, max, CK_PIPE_F_BLOCK, &n);
            data = rbig;
        } else {
            err = ck_stream_write(&pool, h->t, h->h, c[2].b, c[2].n, CK_PIPE_F_BLOCK, &n);
        }
        expect(act_err == OSH_E_OK, cur_file, cur_line, "the wait hook's action succeeded");
    } else if (!strcmp(c[0].s, "frame") && nc >= 3 && table_of(c[1].s)) {
        uint64_t cells[16];
        size_t ncell = 0;
        for (int i = 3; i < nc && ncell < 16; i++) {
            if (c[i].s[0] == '$') {
                struct name *x = find(c[i].s + 1);
                if (!expect(x != 0, cur_file, cur_line, "frame names a known handle"))
                    return;
                cells[ncell++] = ck_handle_raw(x->h);
            } else {
                cells[ncell++] = strtoull(c[i].s, 0, 16);
            }
        }
        struct ck_pipe_ret ret;
        struct ck_cap_table *me = table_of(c[1].s);
        uint8_t slice[BYTES_MAX];
        size_t sl = 0;
        if (c[2].quoted) {
            memcpy(slice, c[2].b, c[2].n);
            sl = c[2].n;
        }
        err = ck_pipe_dispatch(&pool, me, me == &ta ? &tb : &ta, cells, ncell, sl ? slice : 0, sl, &ret);
        expect(ret.error == err, cur_file, cur_line, "frame result carries the error");
        n = (size_t)ret.value;
        if (want_n < 0)
            n = 0;
    } else {
        int hb;
        if (!expect(run_cmd(c, nc, &err, &n, &data, &hb) == 0, cur_file, cur_line, "known command"))
            return;
    }
    snprintf(what, sizeof what, "%s: want %s got %s", c[0].s, osh_err_label((unsigned)want), osh_err_label(err));
    expect(err == (unsigned)want, cur_file, cur_line, what);
    if (want_n >= 0) {
        snprintf(what, sizeof what, "%s: want n=%ld got n=%zu", c[0].s, want_n, n);
        expect(n == (size_t)want_n, cur_file, cur_line, what);
    }
    if (want_data)
        expect(data && n == want_data->n && !memcmp(data, want_data->b, n), cur_file, cur_line, "data byte for byte");
}

static int run_file(const char *dir, const char *fname)
{
    char path[512], line[1024];
    snprintf(path, sizeof path, "%s/%s", dir, fname);
    FILE *f = fopen(path, "r");
    if (!expect(f != 0, fname, 0, "fixture opens"))
        return 0;
    ck_cap_init(&ta, 0x41, CK_CAP_SLOTS);
    ck_cap_init(&tb, 0x42, CK_CAP_SLOTS);
    ck_pipe_pool_init(&pool, &vec_hooks);
    nnames = 0;
    cur_file = fname;
    cur_line = 0;
    int has_case = 0, has_kind = 0, fail_before = ck_t_fail;
    struct tok t[TOK_MAX];
    while (fgets(line, sizeof line, f)) {
        cur_line++;
        int nt = tokenize(line, t, TOK_MAX);
        if (!expect(nt >= 0, fname, cur_line, "line tokenizes"))
            continue;
        if (nt == 0)
            continue;
        has_case |= !strcmp(t[0].s, "case") && nt == 2;
        has_kind |= !strcmp(t[0].s, "kind") && nt == 2 && (!strcmp(t[1].s, "positive") || !strcmp(t[1].s, "negative"));
        run_line(t, nt);
    }
    fclose(f);
    expect(has_case && has_kind, fname, cur_line, "fixture names its case and kind");
    expect(all_empty(), fname, cur_line, "end of case: both tables and the pool are empty");
    printf("  pipe fixture %s: %s\n", fname, ck_t_fail == fail_before ? "ok" : "FAILED");
    return 1;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void run_fixtures(const char *dir)
{
    DIR *d = opendir(dir);
    if (!expect(d != 0, dir, 0, "fixture directory opens"))
        return;
    char *list[128];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 128) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".vec"))
            list[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(list, (size_t)n, sizeof list[0], cmp_str);
    int ran = 0;
    for (int i = 0; i < n; i++) {
        ran += run_file(dir, list[i]);
        free(list[i]);
    }
    CHECK(ran >= 20);
}

/* ---- transfers through the scheduler ---- */
static struct pd_world world;
static uint8_t sink[200000];

static size_t expected_nuls(size_t n)
{
    size_t z = 0;
    for (size_t i = 0; i < n; i++)
        z += pd_byte(i) == 0;
    return z;
}

static void fresh_tables(void)
{
    ck_cap_init(&ta, 0x41, CK_CAP_SLOTS);
    ck_cap_init(&tb, 0x42, CK_CAP_SLOTS);
}

static void check_full_transfer(const char *label, unsigned flags, size_t cw, size_t cr, unsigned pe, unsigned ce)
{
    fresh_tables();
    memset(sink, 0xaa, sizeof sink);
    pd_run(&world, &pool, &ta, &tb, sink, sizeof sink, cw, cr, flags, pe, ce, 0);
    const struct ck_pipe_stats *s = &pool.total;
    printf("  transfer %s: sent=%zu recvd=%zu nul=%zu would_block=%llu waits=%llu wakeups=%llu max_resident=%llu "
           "prod_steps=%llu cons_steps=%llu ticks=%llu\n",
           label, world.sent, world.recvd, pd_nul_count(&world), (unsigned long long)s->would_block,
           (unsigned long long)s->waits, (unsigned long long)s->wakeups, (unsigned long long)s->max_resident,
           (unsigned long long)world.prod_steps, (unsigned long long)world.cons_steps,
           (unsigned long long)world.ticks);
    CHECK(world.setup_err == OSH_E_OK);
    CHECK(world.prod_err == OSH_E_OK && world.cons_err == OSH_E_OK);
    CHECK(world.tasks[0].done && world.tasks[1].done);
    CHECK(world.sent == sizeof sink && world.recvd == sizeof sink);
    CHECK(pd_mismatches(&world) == 0);                                  /* byte for byte */
    CHECK(pd_nul_count(&world) == expected_nuls(sizeof sink));          /* NUL bytes included */
    CHECK(pd_nul_count(&world) >= sizeof sink / 5);
    CHECK(world.eof_seen && s->eof == 1);                               /* last writer closed -> EOF */
    CHECK(s->bytes_written == sizeof sink && s->bytes_read == sizeof sink);
    CHECK(s->max_resident <= CK_PIPE_CAPACITY);                         /* bounded memory */
    CHECK(s->cancellations == 0 && s->broken_pipe == 0 && s->revocations == 0);
    CHECK(world.prod_steps > 1 && world.cons_steps > 1);                /* real interleaving */
    CHECK(all_empty());
}

static void test_transfers(void)
{
    /* slow consumer, non-blocking: the producer sees WOULD_BLOCK and the buffer reaches capacity */
    check_full_transfer("slow-consumer-nonblocking", 0, 8192, 1000, 1, 4);
    CHECK(pool.total.would_block > 0);
    CHECK(pool.total.max_resident == CK_PIPE_CAPACITY);
    CHECK(pool.total.waits == 0);
    /* slow consumer, blocking: the producer parks and the consumer's reads wake it */
    check_full_transfer("slow-consumer-blocking", CK_PIPE_F_BLOCK, 8192, 1000, 1, 4);
    CHECK(pool.total.waits > 0 && pool.total.wakeups > 0);
    CHECK(pool.total.max_resident == CK_PIPE_CAPACITY);
    /* slow producer, blocking: the consumer parks on an empty pipe and the producer's writes wake it */
    check_full_transfer("slow-producer-blocking", CK_PIPE_F_BLOCK, 1000, 8192, 4, 1);
    CHECK(pool.total.waits > 0 && pool.total.wakeups > 0);
    CHECK(pool.total.max_resident < CK_PIPE_CAPACITY);
}

static void test_revoke_midstream(unsigned flags)
{
    fresh_tables();
    memset(sink, 0xaa, sizeof sink);
    pd_run(&world, &pool, &ta, &tb, sink, sizeof sink, 8192, 1000, flags, 1, 4, 50000);
    const struct ck_pipe_stats *s = &pool.total;
    printf("  revoke mid-stream (%s): recvd=%zu sent=%zu reads_after_revoke=%zu prod_err=%s cons_err=%s\n",
           flags ? "blocking" : "nonblocking", world.recvd, world.sent, world.reads_after_revoke,
           osh_err_label(world.prod_err), osh_err_label(world.cons_err));
    CHECK(world.setup_err == OSH_E_OK);
    CHECK(world.revoked);
    CHECK(world.reads_after_revoke == 0);                 /* no data emitted after revocation */
    CHECK(world.recvd >= 50000 && world.recvd < 50000 + 1000);
    CHECK(s->bytes_read == world.recvd);
    CHECK(world.cons_err == OSH_E_REVOKED && world.prod_err == OSH_E_REVOKED);
    CHECK(world.sent < sizeof sink);
    CHECK(pd_mismatches(&world) == 0);
    CHECK(s->revocations == 1 && s->eof == 0);
    CHECK(world.tasks[0].done && world.tasks[1].done);
    CHECK(all_empty());
}

static void test_epoch_and_forgery(void)
{
    fresh_tables();
    ck_pipe_pool_init(&pool, 0);
    struct ck_handle r, w, f, g;
    size_t n = 0;
    uint8_t b[4] = { 1, 2, 3, 4 };
    CHECK(ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_OK);
    uint32_t epoch = pool.pipes[0].epoch;
    CHECK(epoch == 1);
    /* a capability naming another incarnation of the same slot is STALE (section 4.4), and close still releases it */
    CHECK(ck_cap_insert(&ta, ck_pipe_resource(0, CK_PIPE_END_WRITE, epoch + 1), CK_R_WRITE, &f) == CK_CAP_OK);
    CHECK(ck_stream_write(&pool, &ta, f, b, 4, 0, &n) == OSH_E_STALE && n == 0);
    CHECK(ck_stream_close(&pool, &ta, f) == OSH_E_STALE);
    CHECK(ck_cap_lookup(&ta, f, 0, 0) == CK_CAP_INVALID);
    CHECK(pool.pipes[0].writer_refs == 1); /* the live pipe's count is untouched */
    /* a capability on something that is not a stream is DENIED and not closed here */
    CHECK(ck_cap_insert(&ta, CK_IPC_OBJECT_RESOURCE, CK_R_WRITE, &g) == CK_CAP_OK);
    CHECK(ck_stream_write(&pool, &ta, g, b, 4, 0, &n) == OSH_E_DENIED);
    CHECK(ck_stream_close(&pool, &ta, g) == OSH_E_DENIED);
    CHECK(ck_cap_remove(&ta, g) == CK_CAP_OK);
    /* no wait hook: a blocking read on an empty pipe returns WOULD_BLOCK */
    CHECK(ck_stream_read(&pool, &ta, r, b, 4, CK_PIPE_F_BLOCK, &n) == OSH_E_WOULD_BLOCK);
    CHECK(ck_stream_read(&pool, &ta, r, b, 4, 0x80, &n) == OSH_E_INVALID_ARG);
    CHECK(ck_stream_close(&pool, &ta, r) == OSH_E_OK && ck_stream_close(&pool, &ta, w) == OSH_E_OK);
    CHECK(all_empty());
    /* a slot whose epoch reached the maximum is retired; with every slot retired create is LIMIT */
    for (unsigned i = 0; i < CK_PIPE_POOL_PIPES; i++)
        pool.pipes[i].epoch = CK_PIPE_EPOCH_MAX - 1;
    CHECK(ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_OK && pool.pipes[0].epoch == CK_PIPE_EPOCH_MAX);
    CHECK(ck_pipe_release_table(&pool, &ta) == 2);
    for (unsigned i = 1; i < CK_PIPE_POOL_PIPES; i++)
        pool.pipes[i].epoch = CK_PIPE_EPOCH_MAX;
    CHECK(ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_LIMIT);
    CHECK(all_empty());
}

static void test_dispatch(void)
{
    fresh_tables();
    ck_pipe_pool_init(&pool, 0);
    struct ck_pipe_ret ret;
    uint64_t create[4] = { CK_PIPE_FRAME_MAGIC, CK_PIPE_ABI_VERSION, CK_PIPE_OP_CREATE, 0 };
    CHECK(ck_pipe_dispatch(&pool, &ta, &tb, create, 4, 0, 0, &ret) == OSH_E_OK && ret.error == OSH_E_OK);
    uint64_t rd = ret.value, wr = ret.value2;
    CHECK(rd != 0 && wr != 0 && rd != wr);
    uint8_t msg[5] = { 'p', 0, 'i', 0, 'p' }, got[8];
    uint64_t wf[6] = { CK_PIPE_FRAME_MAGIC, CK_PIPE_ABI_VERSION, CK_PIPE_OP_WRITE, 0, wr, 0 };
    CHECK(ck_pipe_dispatch(&pool, &ta, &tb, wf, 6, msg, 5, &ret) == OSH_E_OK && ret.value == 5);
    uint64_t dup[7] = { CK_PIPE_FRAME_MAGIC, CK_PIPE_ABI_VERSION, CK_PIPE_OP_DUP, 0, rd, CK_R_READ, 1 };
    CHECK(ck_pipe_dispatch(&pool, &ta, &tb, dup, 7, 0, 0, &ret) == OSH_E_OK);
    uint64_t rb = ret.value;
    uint64_t rf[6] = { CK_PIPE_FRAME_MAGIC, CK_PIPE_ABI_VERSION, CK_PIPE_OP_READ, 0, rb, 0 };
    /* the dup lives in the peer table: from the caller's table the same raw handle is not ours */
    CHECK(ck_pipe_dispatch(&pool, &tb, &ta, rf, 6, got, 8, &ret) == OSH_E_OK && ret.value == 5 && !memcmp(got, msg, 5));
    /* the read end in the peer table cannot write */
    uint64_t wb[6] = { CK_PIPE_FRAME_MAGIC, CK_PIPE_ABI_VERSION, CK_PIPE_OP_WRITE, 0, rb, 0 };
    CHECK(ck_pipe_dispatch(&pool, &tb, &ta, wb, 6, msg, 5, &ret) == OSH_E_DENIED && ret.value == 0);
    uint64_t cl[5] = { CK_PIPE_FRAME_MAGIC, CK_PIPE_ABI_VERSION, CK_PIPE_OP_CLOSE, 0, 0 };
    cl[4] = rd;
    CHECK(ck_pipe_dispatch(&pool, &ta, &tb, cl, 5, 0, 0, &ret) == OSH_E_OK);
    cl[4] = wr;
    CHECK(ck_pipe_dispatch(&pool, &ta, &tb, cl, 5, 0, 0, &ret) == OSH_E_OK);
    cl[4] = rb;
    CHECK(ck_pipe_dispatch(&pool, &tb, &ta, cl, 5, 0, 0, &ret) == OSH_E_OK);
    CHECK(all_empty());
    CHECK(ck_pipe_dispatch(&pool, &ta, &tb, 0, 0, 0, 0, &ret) == OSH_E_INVALID_ARG);
}

/* A close that fails must not add to the release count. The forged cap is removed (STALE) and still not counted.
 * A pipe-tagged index outside the pool is DENIED, left in the table, and not counted. */
static void test_release_count(void)
{
    fresh_tables();
    ck_pipe_pool_init(&pool, 0);
    struct ck_handle r, w, forged, outside;
    CHECK(ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_OK);
    uint32_t epoch = pool.pipes[0].epoch;
    CHECK(ck_cap_insert(&ta, ck_pipe_resource(0, CK_PIPE_END_READ, epoch + 1), CK_R_READ, &forged) == CK_CAP_OK);
    CHECK(ck_cap_insert(&ta, ck_pipe_resource(7, CK_PIPE_END_WRITE, 1), CK_R_WRITE, &outside) == CK_CAP_OK);
    CHECK(ck_pipe_release_table(&pool, &ta) == 2);
    CHECK(ck_cap_lookup(&ta, forged, 0, 0) == CK_CAP_INVALID);
    CHECK(ck_cap_lookup(&ta, outside, 0, 0) == CK_CAP_OK);
    CHECK(ck_cap_remove(&ta, outside) == CK_CAP_OK);
    CHECK(all_empty());
}

/* Blocking write must commit the bytes it snapshotted, not a buffer the peer rewrote while the writer was parked. */
static uint8_t toctou_src[4] = { 1, 2, 3, 4 };
static struct ck_handle toctou_src_rd;

static unsigned toctou_wait(void *ctx, unsigned pipe, unsigned end)
{
    (void)ctx;
    (void)pipe;
    (void)end;
    memset(toctou_src, 0xA5, sizeof toctou_src);
    uint8_t dump[4];
    size_t n = 0;
    unsigned e = ck_stream_read(&pool, &ta, toctou_src_rd, dump, 4, 0, &n);
    if (e != OSH_E_OK || n != 4)
        return OSH_E_IO;
    return OSH_E_OK;
}

static void toctou_wake(void *ctx, unsigned pipe, unsigned end)
{
    (void)ctx;
    (void)pipe;
    (void)end;
}

static void test_toctou_snapshot(void)
{
    fresh_tables();
    static const struct ck_pipe_hooks hooks = { 0, toctou_wait, toctou_wake, 0 };
    ck_pipe_pool_init(&pool, &hooks);
    struct ck_handle r, w;
    size_t n = 0;
    CHECK(ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_OK);
    toctou_src_rd = r;
    memset(big, 0x11, CK_PIPE_CAPACITY);
    CHECK(ck_stream_write(&pool, &ta, w, big, CK_PIPE_CAPACITY, 0, &n) == OSH_E_OK && n == CK_PIPE_CAPACITY);
    toctou_src[0] = 1;
    toctou_src[1] = 2;
    toctou_src[2] = 3;
    toctou_src[3] = 4;
    CHECK(ck_stream_write(&pool, &ta, w, toctou_src, 4, CK_PIPE_F_BLOCK, &n) == OSH_E_OK && n == 4);
    CHECK(toctou_src[0] == 0xA5 && toctou_src[3] == 0xA5);
    int bad = 0;
    for (size_t got = 0; got < CK_PIPE_CAPACITY;) {
        uint8_t b[64];
        size_t want = CK_PIPE_CAPACITY - got;
        if (want > sizeof b)
            want = sizeof b;
        size_t rn = 0;
        if (ck_stream_read(&pool, &ta, r, b, want, 0, &rn) != OSH_E_OK || rn != want) {
            bad = 1;
            break;
        }
        for (size_t i = 0; i < rn; i++) {
            uint8_t expect = (got + i) < CK_PIPE_CAPACITY - 4 ? 0x11 : (uint8_t)(got + i - (CK_PIPE_CAPACITY - 4) + 1);
            if (b[i] != expect)
                bad = 1;
        }
        got += rn;
    }
    CHECK(!bad);
    CHECK(ck_stream_close(&pool, &ta, r) == OSH_E_OK && ck_stream_close(&pool, &ta, w) == OSH_E_OK);
    CHECK(all_empty());
}

/* Host threads. The wait hook is a counting semaphore: a wake that lands before wait runs is still observed. */
struct host_sync {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned pending[CK_PIPE_POOL_PIPES][2];
    int in_window, peer_done, use_window, timed_out;
};

static struct host_sync host;

static void on_alarm(int sig)
{
    (void)sig;
    const char msg[] = "test_pipe: timeout (lock or wakeup)\n";
    ssize_t wr = write(STDERR_FILENO, msg, sizeof msg - 1);
    (void)wr;
    _Exit(1);
}

static void host_lock(void)
{
    pthread_mutex_lock(&host.mu);
}

static void host_unlock(void)
{
    pthread_mutex_unlock(&host.mu);
}

static int wait_pred(int *pred)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    while (!*pred) {
        if (pthread_cond_timedwait(&host.cv, &host.mu, &ts) == ETIMEDOUT)
            return -1;
    }
    return 0;
}

static void host_before(void *ctx)
{
    struct host_sync *s = ctx;
    if (!s->use_window)
        return;
    host_lock();
    s->in_window = 1;
    pthread_cond_broadcast(&s->cv);
    int bad = wait_pred(&s->peer_done);
    if (bad)
        s->timed_out = 1;
    host_unlock();
}

static unsigned host_wait(void *ctx, unsigned pipe, unsigned end)
{
    struct host_sync *s = ctx;
    host_lock();
    if (!s->use_window) {
        s->in_window = 1;
        pthread_cond_broadcast(&s->cv);
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    while (s->pending[pipe][end] == 0) {
        if (pthread_cond_timedwait(&s->cv, &s->mu, &ts) == ETIMEDOUT) {
            s->timed_out = 1;
            host_unlock();
            return OSH_E_INTERRUPTED;
        }
    }
    s->pending[pipe][end]--;
    host_unlock();
    return OSH_E_OK;
}

static void host_wake(void *ctx, unsigned pipe, unsigned end)
{
    struct host_sync *s = ctx;
    host_lock();
    s->pending[pipe][end]++;
    pthread_cond_broadcast(&s->cv);
    host_unlock();
}

static void host_reset(int window)
{
    host_lock();
    memset(host.pending, 0, sizeof host.pending);
    host.in_window = host.peer_done = host.timed_out = 0;
    host.use_window = window;
    host_unlock();
}

static int wait_window(void)
{
    host_lock();
    int bad = wait_pred(&host.in_window);
    host_unlock();
    return bad;
}

static void mark_peer(void)
{
    host_lock();
    host.peer_done = 1;
    pthread_cond_broadcast(&host.cv);
    host_unlock();
}

struct xfer {
    struct ck_cap_table *table;
    struct ck_handle h;
    size_t total, chunk, moved, bad;
    unsigned err;
    int reader;
};

static void *xfer_thread(void *arg)
{
    struct xfer *x = arg;
    size_t off = 0;
    while (off < x->total) {
        size_t want = x->total - off;
        if (want > x->chunk)
            want = x->chunk;
        uint8_t tmp[1024];
        size_t n = 0;
        unsigned e;
        if (x->reader) {
            e = ck_stream_read(&pool, x->table, x->h, tmp, want, CK_PIPE_F_BLOCK, &n);
            if (e != OSH_E_OK || n == 0) {
                x->err = e == OSH_E_OK ? 0xffu : e;
                return 0;
            }
            for (size_t i = 0; i < n; i++)
                x->bad += tmp[i] != pd_byte(off + i);
        } else {
            for (size_t i = 0; i < want; i++)
                tmp[i] = pd_byte(off + i);
            e = ck_stream_write(&pool, x->table, x->h, tmp, want, CK_PIPE_F_BLOCK, &n);
            if (e != OSH_E_OK || n == 0) {
                x->err = e == OSH_E_OK ? 0xffu : e;
                return 0;
            }
        }
        off += n;
    }
    x->moved = off;
    if (x->reader) {
        size_t n = 0;
        uint8_t b;
        unsigned e = ck_stream_read(&pool, x->table, x->h, &b, 1, CK_PIPE_F_BLOCK, &n);
        if (e != OSH_E_OK || n != 0)
            x->err = e == OSH_E_OK ? 0xffu : e;
        ck_stream_close(&pool, x->table, x->h);
    } else {
        unsigned e = ck_stream_close(&pool, x->table, x->h);
        if (e != OSH_E_OK)
            x->err = e;
    }
    return 0;
}

static void *nb_thread(void *arg)
{
    struct xfer *x = arg;
    size_t off = 0;
    while (off < x->total) {
        size_t want = x->total - off;
        if (want > x->chunk)
            want = x->chunk;
        uint8_t tmp[256];
        size_t n = 0;
        unsigned e;
        if (x->reader) {
            e = ck_stream_read(&pool, x->table, x->h, tmp, want, 0, &n);
            if (e == OSH_E_WOULD_BLOCK) {
                sched_yield();
                continue;
            }
            if (e != OSH_E_OK || n == 0) {
                x->err = e == OSH_E_OK ? 0xffu : e;
                return 0;
            }
            for (size_t i = 0; i < n; i++)
                x->bad += tmp[i] != pd_byte(off + i);
        } else {
            for (size_t i = 0; i < want; i++)
                tmp[i] = pd_byte(off + i);
            e = ck_stream_write(&pool, x->table, x->h, tmp, want, 0, &n);
            if (e == OSH_E_WOULD_BLOCK) {
                sched_yield();
                continue;
            }
            if (e != OSH_E_OK || n == 0) {
                x->err = e == OSH_E_OK ? 0xffu : e;
                return 0;
            }
        }
        off += n;
    }
    x->moved = off;
    if (!x->reader) {
        unsigned e = ck_stream_close(&pool, x->table, x->h);
        if (e != OSH_E_OK)
            x->err = e;
    } else {
        for (;;) {
            uint8_t b;
            size_t n = 0;
            unsigned e = ck_stream_read(&pool, x->table, x->h, &b, 1, 0, &n);
            if (e == OSH_E_WOULD_BLOCK) {
                sched_yield();
                continue;
            }
            if (e != OSH_E_OK || n != 0)
                x->err = e == OSH_E_OK ? 0xffu : e;
            break;
        }
        ck_stream_close(&pool, x->table, x->h);
    }
    return 0;
}

struct pair_job {
    struct ck_cap_table *table;
    unsigned err;
    int n;
};

static void *pair_thread(void *arg)
{
    struct pair_job *j = arg;
    for (int i = 0; i < j->n; i++) {
        struct ck_handle r, w;
        unsigned e = ck_pipe_create(&pool, j->table, &r, &w);
        if (e != OSH_E_OK) {
            j->err = e;
            return 0;
        }
        uint8_t b[8], out[8];
        for (int k = 0; k < 8; k++)
            b[k] = (uint8_t)(i + k);
        size_t n = 0;
        e = ck_stream_write(&pool, j->table, w, b, 8, 0, &n);
        if (e != OSH_E_OK || n != 8) {
            j->err = e == OSH_E_OK ? 0xffu : e;
            return 0;
        }
        e = ck_stream_read(&pool, j->table, r, out, 8, 0, &n);
        if (e != OSH_E_OK || n != 8 || memcmp(out, b, 8) != 0) {
            j->err = e == OSH_E_OK ? 0xffu : e;
            return 0;
        }
        if (ck_stream_close(&pool, j->table, r) != OSH_E_OK || ck_stream_close(&pool, j->table, w) != OSH_E_OK) {
            j->err = OSH_E_IO;
            return 0;
        }
    }
    return 0;
}

struct one_call {
    struct ck_cap_table *table;
    struct ck_handle h;
    int is_write;
    unsigned err;
    size_t n;
    uint8_t buf[4];
};

static void *one_call_thread(void *arg)
{
    struct one_call *c = arg;
    if (c->is_write)
        c->err = ck_stream_write(&pool, c->table, c->h, c->buf, 4, CK_PIPE_F_BLOCK, &c->n);
    else
        c->err = ck_stream_read(&pool, c->table, c->h, c->buf, 4, CK_PIPE_F_BLOCK, &c->n);
    return 0;
}

static void join_one(pthread_t th, struct one_call *c, unsigned want, size_t want_n)
{
    pthread_join(th, 0);
    CHECK(c->err == want);
    CHECK(c->n == want_n);
    CHECK(!host.timed_out);
}

static void test_close_park(int window, int break_writer)
{
    fresh_tables();
    host_reset(window);
    struct ck_pipe_hooks hooks = { &host, host_wait, host_wake, host_before };
    ck_pipe_pool_init(&pool, &hooks);
    struct ck_handle r, w;
    CHECK(ck_pipe_create(&pool, &ta, &r, &w) == OSH_E_OK);
    struct one_call call;
    memset(&call, 0, sizeof call);
    pthread_t th;
    if (break_writer) {
        size_t n = 0;
        memset(big, 0x22, CK_PIPE_CAPACITY);
        CHECK(ck_stream_write(&pool, &ta, w, big, CK_PIPE_CAPACITY, 0, &n) == OSH_E_OK && n == CK_PIPE_CAPACITY);
        call.table = &ta;
        call.h = w;
        call.is_write = 1;
        call.buf[0] = call.buf[1] = call.buf[2] = call.buf[3] = 9;
        CHECK(pthread_create(&th, 0, one_call_thread, &call) == 0);
        CHECK(wait_window() == 0);
        CHECK(ck_stream_close(&pool, &ta, r) == OSH_E_OK);
        if (window)
            mark_peer();
        join_one(th, &call, OSH_E_BROKEN_PIPE, 0);
        CHECK(pool.total.bytes_written == CK_PIPE_CAPACITY);
        CHECK(ck_stream_close(&pool, &ta, w) == OSH_E_OK);
    } else {
        call.table = &ta;
        call.h = r;
        CHECK(pthread_create(&th, 0, one_call_thread, &call) == 0);
        CHECK(wait_window() == 0);
        CHECK(ck_stream_close(&pool, &ta, w) == OSH_E_OK);
        if (window)
            mark_peer();
        join_one(th, &call, OSH_E_OK, 0);
        CHECK(pool.total.eof == 1);
        CHECK(ck_stream_close(&pool, &ta, r) == OSH_E_OK);
    }
    CHECK(all_empty());
}

static void test_concurrent(void)
{
    signal(SIGALRM, on_alarm);
    alarm(60);
    static const struct ck_pipe_hooks hooks = { &host, host_wait, host_wake, host_before };
    const size_t total = 128u * 1024u;
    for (int iter = 0; iter < 4; iter++) {
        fresh_tables();
        host_reset(0);
        ck_pipe_pool_init(&pool, &hooks);
        struct ck_handle r0, w, r;
        CHECK(ck_pipe_create(&pool, &ta, &r0, &w) == OSH_E_OK);
        CHECK(ck_stream_dup(&pool, &ta, r0, &tb, CK_R_READ, &r) == OSH_E_OK);
        CHECK(ck_stream_close(&pool, &ta, r0) == OSH_E_OK);
        struct xfer wr = { .table = &ta, .h = w, .total = total, .chunk = 1000 };
        struct xfer rd = { .table = &tb, .h = r, .total = total, .chunk = 700, .reader = 1 };
        pthread_t tw, tr;
        CHECK(pthread_create(&tw, 0, xfer_thread, &wr) == 0);
        CHECK(pthread_create(&tr, 0, xfer_thread, &rd) == 0);
        pthread_join(tw, 0);
        pthread_join(tr, 0);
        CHECK(wr.err == 0 && rd.err == 0);
        CHECK(wr.moved == total && rd.moved == total && rd.bad == 0);
        CHECK(!host.timed_out);
        CHECK(all_empty());
    }
    fresh_tables();
    ck_pipe_pool_init(&pool, 0);
    struct ck_handle r0, w, r;
    CHECK(ck_pipe_create(&pool, &ta, &r0, &w) == OSH_E_OK);
    CHECK(ck_stream_dup(&pool, &ta, r0, &tb, CK_R_READ, &r) == OSH_E_OK);
    CHECK(ck_stream_close(&pool, &ta, r0) == OSH_E_OK);
    struct xfer wr = { .table = &ta, .h = w, .total = total, .chunk = 256 };
    struct xfer rd = { .table = &tb, .h = r, .total = total, .chunk = 128, .reader = 1 };
    pthread_t tw, tr;
    CHECK(pthread_create(&tw, 0, nb_thread, &wr) == 0);
    CHECK(pthread_create(&tr, 0, nb_thread, &rd) == 0);
    pthread_join(tw, 0);
    pthread_join(tr, 0);
    CHECK(wr.err == 0 && rd.err == 0);
    CHECK(wr.moved == total && rd.moved == total && rd.bad == 0);
    CHECK(all_empty());

    test_close_park(1, 0);
    test_close_park(1, 1);
    test_close_park(0, 0);
    test_close_park(0, 1);

    fresh_tables();
    ck_pipe_pool_init(&pool, 0);
    struct pair_job ja = { .table = &ta, .n = 80 };
    struct pair_job jb = { .table = &tb, .n = 80 };
    CHECK(pthread_create(&tw, 0, pair_thread, &ja) == 0);
    CHECK(pthread_create(&tr, 0, pair_thread, &jb) == 0);
    pthread_join(tw, 0);
    pthread_join(tr, 0);
    CHECK(ja.err == 0 && jb.err == 0);
    CHECK(all_empty());
    alarm(0);
}

int main(int argc, char **argv)
{
    _Static_assert(CK_PIPE_CAPACITY >= 65536u, "section 8.5 minimum");
    CHECK(osh_err_label(OSH_E_WOULD_BLOCK)[0] == 'W' && !strcmp(osh_err_label(99), "IO"));
    run_fixtures(argc > 1 ? argv[1] : PIPE_FIXTURE_DIR);
    test_transfers();
    test_revoke_midstream(0);
    test_revoke_midstream(CK_PIPE_F_BLOCK);
    test_epoch_and_forgery();
    test_dispatch();
    test_release_count();
    test_toctou_snapshot();
    CHECK(pthread_mutex_init(&host.mu, 0) == 0);
    CHECK(pthread_cond_init(&host.cv, 0) == 0);
    test_concurrent();
    pthread_cond_destroy(&host.cv);
    pthread_mutex_destroy(&host.mu);
    return ck_t_verdict("test_pipe");
}
