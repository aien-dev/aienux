/* allen_memory_test.c -- host tests for Allen's lasting memory:
 *   1. one copy encodes and decodes; every kind of damage is refused;
 *   2. the two-copy store: first start empty, saves alternate, a power cut
 *      in the middle of a save keeps the previous memory, a spoiled newest
 *      copy falls back to the older one;
 *   3. the AIENOS partition added in unused space (dev/gpt_add.c) next to an
 *      existing partition full of sentinel bytes: only the GPT blocks change,
 *      a second run finds it, no space / no GPT / a torn add write nothing;
 *   4. the store on that partition view: nothing outside the partition moves;
 *   5. Allen taught, "restarted" from the bytes on disk, and asked.
 * Hosted test code only (libc). Run by scripts/ck_allen_memory_test.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "allen_memory.h"
#include "allen_reply.h"
#include "disk_file.h"
#include "sha256.h"
#include "disk_part.h"
#include "gpt_add.h"
#include "gpt_write.h"

static int checks, failures;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) {                                                              \
            failures++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                \
        }                                                                        \
    } while (0)

static char g_path[512];
#define MIB(bs) ((1024u * 1024u) / (bs))

static uint8_t *slurp(size_t *len)
{
    FILE *fp = fopen(g_path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, fp) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(fp);
    *len = (size_t)n;
    return b;
}

/* Every block of a and b that differs lies in [lo, hi] (or in [lo2, hi2]). */
static int diff_only_in(const uint8_t *a, const uint8_t *b, size_t len, uint32_t bs, uint64_t lo, uint64_t hi,
                        uint64_t lo2, uint64_t hi2)
{
    for (size_t blk = 0; blk < len / bs; blk++)
        if (memcmp(a + blk * bs, b + blk * bs, bs) && !(blk >= lo && blk <= hi) && !(blk >= lo2 && blk <= hi2)) {
            printf("  block %zu changed outside the allowed ranges\n", blk);
            return 0;
        }
    return 1;
}

static void memory_sample(ck_allen_memory *m)
{
    memset(m, 0, sizeof *m);
    m->starts = 3;
    m->lines = 41;
    strcpy(m->name, "Drake");
    m->nfacts = 2;
    strcpy(m->fact[0].key, "my favorite color");
    strcpy(m->fact[0].val, "blue");
    strcpy(m->fact[1].key, "the dogs");
    strcpy(m->fact[1].val, "rex and max");
    m->fact[1].are = 1;
}

static void test_codec(void)
{
    static uint8_t u[CK_ALLEN_MEM_UNIT], v[CK_ALLEN_MEM_UNIT];
    ck_allen_memory a, b;
    uint64_t gen = 0;
    memory_sample(&a);
    ck_allen_mem_encode(&a, 7, u);
    CHECK(ck_allen_mem_decode(u, &b, &gen) == 0);
    CHECK(gen == 7 && !memcmp(&a, &b, sizeof a));
    /* any flipped byte in the header or payload is refused */
    size_t spots[] = {0, 8, 12, 16, 25, 40, 64, 70, 72 + 5, 100, 200, 300, 4000};
    for (size_t i = 0; i < sizeof spots / sizeof spots[0]; i++) {
        memcpy(v, u, sizeof u);
        v[spots[i]] ^= 0x20;
        CHECK(ck_allen_mem_decode(v, &b, &gen) != 0);
    }
    /* well sealed but bad text, count or generation: still refused */
    ck_allen_memory bad = a;
    strcpy(bad.fact[0].key, "My Color");
    ck_allen_mem_encode(&bad, 1, v);
    CHECK(ck_allen_mem_decode(v, &b, &gen) != 0);
    bad = a;
    strcpy(bad.fact[0].val, "two  spaces");
    ck_allen_mem_encode(&bad, 1, v);
    CHECK(ck_allen_mem_decode(v, &b, &gen) != 0);
    bad = a;
    strcpy(bad.name, "drake");
    ck_allen_mem_encode(&bad, 1, v);
    CHECK(ck_allen_mem_decode(v, &b, &gen) != 0);
    bad = a;
    bad.fact[1].key[0] = 0;
    ck_allen_mem_encode(&bad, 1, v);
    CHECK(ck_allen_mem_decode(v, &b, &gen) != 0);
    ck_allen_mem_encode(&a, 0, v);
    CHECK(ck_allen_mem_decode(v, &b, &gen) != 0);
    memset(v, 0, sizeof v);
    CHECK(ck_allen_mem_decode(v, &b, &gen) != 0);
    /* a full memory fits and round-trips */
    ck_allen_memory full;
    memset(&full, 0, sizeof full);
    full.nfacts = CK_ALLEN_FACTS_MAX;
    for (unsigned i = 0; i < CK_ALLEN_FACTS_MAX; i++) {
        memset(full.fact[i].key, 'k', CK_ALLEN_KEY_MAX - 1);
        full.fact[i].key[0] = (char)('a' + i % 26);
        memset(full.fact[i].val, 'v', CK_ALLEN_VAL_MAX - 1);
    }
    ck_allen_mem_encode(&full, 9, u);
    CHECK(ck_allen_mem_decode(u, &b, &gen) == 0 && !memcmp(&full, &b, sizeof full));
    /* Allen's own name rides along (version 3), and a version 2 unit written
     * before he had one still reads, with no name. */
    ck_allen_memory named = a;
    strcpy(named.self, "Nova");
    ck_allen_mem_encode(&named, 11, u);
    CHECK(ck_allen_mem_decode(u, &b, &gen) == 0 && gen == 11 && !strcmp(b.self, "Nova") && !strcmp(b.name, "Drake"));
    strcpy(named.self, "nova");
    ck_allen_mem_encode(&named, 11, v);
    CHECK(ck_allen_mem_decode(v, &b, &gen) != 0); /* a name starts with a capital, like the person's */
    ck_allen_mem_encode(&a, 5, v);
    {
        /* the same memory as version 2: no self field, shorter payload, resealed */
        uint32_t plen = 0;
        for (int i = 3; i >= 0; i--)
            plen = plen << 8 | v[12 + i];
        plen -= CK_ALLEN_NAME_MAX + 12u; /* v2 has neither self (24B) nor sky fields (12B) */
        v[8] = 2, v[9] = v[10] = v[11] = 0;
        for (int i = 0; i < 4; i++)
            v[12 + i] = (uint8_t)(plen >> (8 * i));
        sha256_ctx h;
        sha256_init(&h);
        sha256_update(&h, v, 32);
        sha256_update(&h, v + 64, plen);
        sha256_final(&h, v + 32);
    }
    CHECK(ck_allen_mem_decode(v, &b, &gen) == 0 && gen == 5 && !b.self[0] && !memcmp(&a, &b, sizeof a));
    v[8] = 5; /* a version from the future is refused */
    CHECK(ck_allen_mem_decode(v, &b, &gen) != 0);
}

/* A memory large enough that one copy spans all 8 blocks of its unit, so a
 * power cut part way really tears it. */
static void memory_big(ck_allen_memory *m)
{
    memory_sample(m);
    m->nfacts = 30;
    for (unsigned i = 2; i < 30; i++) {
        snprintf(m->fact[i].key, sizeof m->fact[i].key, "fact number %u", i);
        snprintf(m->fact[i].val, sizeof m->fact[i].val, "value %u", i * 7);
    }
}

static void test_store(void)
{
    disk_file f;
    disk_dev d;
    ck_allen_store st;
    ck_allen_memory m, got;
    CHECK(disk_file_open(&f, &d, g_path, 512, 64, 1) == 0);
    CHECK(ck_allen_store_load(&st, &d, &got) == CK_ALLEN_MEM_EMPTY && st.valid == 0 && got.nfacts == 0);
    memory_big(&m);
    CHECK(ck_allen_store_save(&st, &m) == CK_ALLEN_MEM_OK && st.gen == 1 && st.newest == 0);
    m.lines = 50;
    CHECK(ck_allen_store_save(&st, &m) == CK_ALLEN_MEM_OK && st.gen == 2 && st.newest == 1);
    CHECK(ck_allen_store_load(&st, &d, &got) == CK_ALLEN_MEM_OK && st.gen == 2 && st.valid == 2 &&
          got.lines == 50);
    /* power cut after 3 of the 8 blocks of the next save */
    m.lines = 60;
    strcpy(m.fact[29].val, "changed at the end of the copy");
    disk_file_arm_cut(&f, 3);
    CHECK(ck_allen_store_save(&st, &m) != CK_ALLEN_MEM_OK);
    disk_file_disarm(&f);
    CHECK(ck_allen_store_load(&st, &d, &got) == CK_ALLEN_MEM_OK && st.gen == 2 && st.valid == 1 &&
          got.lines == 50);
    /* power cut before the flush: the copy is whole but the flush failed;
     * the save reports failure, and loading takes whichever copy is valid */
    disk_file_arm_cut(&f, 8);
    CHECK(ck_allen_store_save(&st, &m) != CK_ALLEN_MEM_OK);
    disk_file_disarm(&f);
    CHECK(ck_allen_store_load(&st, &d, &got) == CK_ALLEN_MEM_OK && (got.lines == 50 || got.lines == 60));
    CHECK(ck_allen_store_save(&st, &m) == CK_ALLEN_MEM_OK);
    uint64_t newest = st.newest, gen = st.gen;
    /* spoil the newest copy on disk: the older one is used */
    disk_file_close(&f);
    FILE *fp = fopen(g_path, "r+b");
    CHECK(fp && fseek(fp, (long)(newest * 4096 + 100), SEEK_SET) == 0 && fputc(0x55, fp) != EOF);
    if (fp)
        fclose(fp);
    CHECK(disk_file_open(&f, &d, g_path, 512, 0, 0) == 0);
    CHECK(ck_allen_store_load(&st, &d, &got) == CK_ALLEN_MEM_OK && st.gen == gen - 1 && st.valid == 1);
    /* the next save goes over the spoiled copy, never over the good one */
    CHECK(ck_allen_store_save(&st, &m) == CK_ALLEN_MEM_OK && st.newest == newest && st.gen == gen);
    CHECK(ck_allen_store_load(&st, &d, &got) == CK_ALLEN_MEM_OK && st.valid == 2 && got.lines == 60);
    disk_file_close(&f);
    /* too small a device is refused */
    CHECK(disk_file_open(&f, &d, g_path, 512, 15, 1) == 0);
    CHECK(ck_allen_store_load(&st, &d, &got) == CK_ALLEN_MEM_EGEOM);
    disk_file_close(&f);
}

static int g_listed;
static char g_names[4][40];
static void on_entry(void *ctx, const ck_gpt_entry *e)
{
    (void)ctx;
    if (g_listed < 4)
        strcpy(g_names[g_listed], e->name);
    g_listed++;
}

static void test_gpt_add(uint32_t bs)
{
    const uint64_t per = MIB(bs), disk = 64 * per;
    /* one existing partition, 1..9 MiB, full of sentinel bytes; the rest free */
    gw_part sentinel[1] = {{gw_linux_type, 1 * per, 9 * per - 1u, "old macOS", 0xa5}};
    CHECK(gw_write(g_path, bs, disk, sentinel, 1, 0) == 0);
    size_t n0 = 0, n1 = 0;
    uint8_t *before = slurp(&n0);
    disk_file f;
    disk_dev d;
    ck_part p;
    ck_gpt_info gi;
    CHECK(disk_file_open(&f, &d, g_path, bs, 0, 0) == 0);
    CHECK(ck_gpt_find_aienos(&d, &p, &gi) == CK_GPT_ENOPART && gi.first_usable && gi.entries == 128);
    g_listed = 0;
    CHECK(ck_gpt_list(&d, on_entry, 0) == 1 && g_listed == 1 && !strcmp(g_names[0], "old macOS"));
    ck_gpt_added a;
    /* 16 MiB on 1 MiB boundaries: the gap before the sentinel is too small,
     * so it lands right after it, at 9 MiB */
    CHECK(ck_gpt_add_aienos(&d, 16 * per, per, &a) == CK_GPT_OK);
    CHECK(a.created == 1 && a.first == 9 * per && a.last == 25 * per - 1u && a.index == 1);
    uint8_t *after = slurp(&n1);
    CHECK(before && after && n0 == n1);
    if (before && after && n0 == n1) {
        /* only the GPT blocks changed: LBA 1 .. first usable - 1 and last usable + 1 .. last */
        CHECK(diff_only_in(before, after, n0, bs, 1, gi.first_usable - 1u, gi.last_usable + 1u, disk - 1u));
        CHECK(!memcmp(before, after, bs)); /* the protective MBR untouched */
        CHECK(!memcmp(before + per * bs, after + per * bs, 8 * per * bs)); /* every sentinel byte */
    }
    CHECK(ck_gpt_find_aienos(&d, &p, &gi) == CK_GPT_OK && p.first_lba == 9 * per && p.blocks == 16 * per);
    g_listed = 0;
    CHECK(ck_gpt_list(&d, on_entry, 0) == 2 && !strcmp(g_names[1], "AIENOS Allen memory"));
    /* a second run finds it and writes nothing */
    free(before);
    before = slurp(&n0);
    CHECK(ck_gpt_add_aienos(&d, 16 * per, per, &a) == CK_GPT_OK && a.created == 0 && a.first == 9 * per);
    free(after);
    after = slurp(&n1);
    CHECK(before && after && n0 == n1 && !memcmp(before, after, n0));
    free(before);
    free(after);

    /* 4. the store on the partition view: nothing outside it moves */
    before = slurp(&n0);
    ck_allen_store st;
    ck_allen_memory m, got;
    memory_sample(&m);
    CHECK(ck_allen_store_load(&st, &p.dev, &got) == CK_ALLEN_MEM_EMPTY);
    CHECK(ck_allen_store_save(&st, &m) == CK_ALLEN_MEM_OK && ck_allen_store_save(&st, &m) == CK_ALLEN_MEM_OK);
    after = slurp(&n1);
    if (before && after && n0 == n1)
        CHECK(diff_only_in(before, after, n0, bs, 9 * per, 9 * per + 2 * 4096 / bs - 1u, 1, 0));
    CHECK(ck_allen_store_load(&st, &p.dev, &got) == CK_ALLEN_MEM_OK && st.valid == 2 && !memcmp(&got, &m, sizeof m));
    free(before);
    free(after);
    disk_file_close(&f);

    /* no unused gap big enough: refused, nothing written */
    gw_part whole[1] = {{gw_linux_type, 34, disk - 34, "whole disk", 0}};
    CHECK(gw_write(g_path, bs, disk, whole, 1, 0) == 0);
    before = slurp(&n0);
    CHECK(disk_file_open(&f, &d, g_path, bs, 0, 0) == 0);
    int rc = ck_gpt_add_aienos(&d, 16 * per, per, &a);
    CHECK(rc == CK_GPT_ENOSPACE || (bs == 4096 && rc == CK_GPT_EHDR)); /* 4096: 34 is inside the array */
    disk_file_close(&f);
    after = slurp(&n1);
    CHECK(before && after && n0 == n1 && !memcmp(before, after, n0));
    free(before);
    free(after);

    /* no GPT at all: refused, nothing written */
    CHECK(gw_write(g_path, bs, disk, sentinel, 1, GW_NO_GPT) == 0);
    before = slurp(&n0);
    CHECK(disk_file_open(&f, &d, g_path, bs, 0, 0) == 0);
    CHECK(ck_gpt_add_aienos(&d, 16 * per, per, &a) == CK_GPT_ENOMBR);
    CHECK(ck_gpt_list(&d, on_entry, 0) == CK_GPT_ENOMBR);
    disk_file_close(&f);
    after = slurp(&n1);
    CHECK(before && after && n0 == n1 && !memcmp(before, after, n0));
    free(before);
    free(after);

    /* torn add: power lost after the backup copy was written. The primary
     * (what the firmware reads) is still the old, valid one; the strict
     * check refuses the pair, and a second attempt writes nothing. */
    CHECK(gw_write(g_path, bs, disk, sentinel, 1, 0) == 0);
    CHECK(disk_file_open(&f, &d, g_path, bs, 0, 0) == 0);
    uint64_t eblocks = (128u * 128u + bs - 1u) / bs;
    disk_file_arm_cut(&f, eblocks + 1u);
    CHECK(ck_gpt_add_aienos(&d, 16 * per, per, &a) == CK_GPT_EWRITE);
    disk_file_disarm(&f);
    CHECK(ck_gpt_find_aienos(&d, &p, &gi) == CK_GPT_EBACKUP);
    before = slurp(&n0);
    CHECK(ck_gpt_add_aienos(&d, 16 * per, per, &a) == CK_GPT_EBACKUP);
    after = slurp(&n1);
    CHECK(before && after && n0 == n1 && !memcmp(before, after, n0));
    free(before);
    free(after);
    disk_file_close(&f);
}

static int says(const char *got, const char *want)
{
    if (strstr(got, want))
        return 1;
    printf("  answer \"%s\" lacks \"%s\"\n", got, want);
    return 0;
}

static void test_teach_restart(void)
{
    static ck_allen_mind m, m2;
    static uint8_t u[CK_ALLEN_MEM_UNIT];
    ck_allen_mind_init(&m, 0);
    m.lasting = 1;
    CHECK(says(ck_allen_welcome(&m), "What will you call me?"));
    CHECK(says(ck_allen_answer(&m, "My name is Drake", 1), "Nice to meet you, Drake. I'll remember that."));
    CHECK(m.changed == 1);
    m.changed = 0;
    CHECK(says(ck_allen_answer(&m, "Remember my favorite color is blue.", 2),
               "Got it: your favorite color is blue. I'll remember that."));
    CHECK(m.changed == 1 && m.mem.nfacts == 1);
    CHECK(says(ck_allen_answer(&m, "remember that the dogs are rex and max", 3), "the dogs are rex and max"));
    CHECK(says(ck_allen_answer(&m, "what is my favorite color", 4), "Your favorite color is blue."));
    CHECK(says(ck_allen_answer(&m, "What are the dogs", 4), "The dogs are rex and max."));
    CHECK(says(ck_allen_answer(&m, "what is my favorite food", 4), "You haven't told me your favorite food yet."));
    CHECK(says(ck_allen_answer(&m, "remember my favorite color is green", 5), "your favorite color is green"));
    CHECK(m.mem.nfacts == 2);
    CHECK(says(ck_allen_answer(&m, "what do you remember", 6),
               "I remember 3 things: your name is Drake; your favorite color is green; the dogs are rex and max."));
    CHECK(says(ck_allen_answer(&m, "remember", 6), "I keep it on this computer's own disk."));
    CHECK(says(ck_allen_answer(&m, "remember blue", 6), "Teach me like this"));
    CHECK(says(ck_allen_answer(&m, "what is 2 plus 2", 6), "2 plus 2 is 4."));

    /* number(): nine digits are the limit; longer digit runs are refused before they can overflow */
    CHECK(says(ck_allen_answer(&m, "what is 999999999 plus 1", 6), "999999999 plus 1 is 1000000000."));
    CHECK(!strstr(ck_allen_answer(&m, "what is 1000000000 plus 1", 6), " is 1000000001"));
    CHECK(!strstr(ck_allen_answer(&m, "what is 1 plus 1000000000", 6), " is 1000000001"));
    CHECK(!strstr(ck_allen_answer(&m, "what is 999999999999999999999999 plus 1", 6), " plus "));
    CHECK(!strstr(ck_allen_answer(&m, "what is 1 times 99999999999999999999999999999", 6), " times "));

    /* "restart": the memory goes to disk bytes and comes back in a new mind */
    m.mem.starts = 1;
    ck_allen_mem_encode(&m.mem, 5, u);
    uint64_t gen = 0;
    ck_allen_mind_init(&m2, 0);
    m2.lasting = 1;
    CHECK(ck_allen_mem_decode(u, &m2.mem, &gen) == 0 && gen == 5);
    m2.mem.starts++;
    CHECK(says(ck_allen_welcome(&m2), "Welcome back, Drake. I still remember 2 things you taught me."));
    CHECK(says(ck_allen_answer(&m2, "what is my favorite color", 1), "Your favorite color is green."));
    CHECK(says(ck_allen_answer(&m2, "what is my name", 1), "You're Drake."));
    CHECK(says(ck_allen_answer(&m2, "how many times have you restarted", 1), "This is start number 2 for me"));
    CHECK(m2.changed == 0);
    CHECK(says(ck_allen_answer(&m2, "forget the dogs", 1), "Done. I've forgotten the dogs."));
    CHECK(m2.changed == 1 && m2.mem.nfacts == 1);
    CHECK(says(ck_allen_answer(&m2, "forget the cat", 1), "I don't remember anything about the cat."));
    CHECK(says(ck_allen_answer(&m2, "forget everything", 1), "forgotten everything"));
    CHECK(m2.mem.nfacts == 0 && !m2.mem.name[0] && says(ck_allen_welcome(&m2), "What will you call me?"));
    CHECK(says(ck_allen_answer(&m2, "what do you remember", 1), "Nothing yet."));

    /* without a disk Allen says it only lasts until power off */
    ck_allen_mind_init(&m, 0);
    CHECK(says(ck_allen_answer(&m, "remember my car is red", 1), "until this computer turns off"));
    /* a full memory says so and keeps what it has */
    for (unsigned i = 0; i < CK_ALLEN_FACTS_MAX + 2; i++) {
        char line[64];
        snprintf(line, sizeof line, "remember thing %u is %u", i, i);
        ck_allen_answer(&m, line, 1);
    }
    CHECK(m.mem.nfacts == CK_ALLEN_FACTS_MAX);
    CHECK(says(ck_allen_answer(&m, "remember one more is here", 1), "My memory is full."));
    /* everything Allen can be taught encodes and decodes */
    ck_allen_mem_encode(&m.mem, 1, u);
    CHECK(ck_allen_mem_decode(u, &m2.mem, &gen) == 0);
    /* too long is refused, not cut */
    ck_allen_mind_init(&m, 0);
    CHECK(says(ck_allen_answer(&m, "remember the name of the very first computer i ever owned back then is x", 1),
               "longer than I can hold"));
    CHECK(m.mem.nfacts == 0);
}


/* Notes: saved as typed, handed back when asked, linked, deleted, and kept
 * on disk one sealed unit each. */
static void test_notes(void)
{
    static ck_allen_mind m, m2;
    static ck_allen_notes ns, back;
    static uint8_t u[CK_ALLEN_MEM_UNIT];
    ck_allen_mind_init(&m, 0);
    memset(&ns, 0, sizeof ns);
    m.notes = &ns;
    m.lasting = 1;
    CHECK(says(ck_allen_answer(&m, "save the gate code is 4471", 1), "Saved. It's your first note."));
    CHECK(m.changed == 1 && m.note_dirty == 1 && m.lit == 1 && m.mem.nnotes == 1 && ns.n == 1);
    m.changed = m.note_dirty = 0;
    CHECK(says(ck_allen_answer(&m, "Note: the wifi password is Blue-Fox-88", 1),
               "Saved. It doesn't link to any other note yet."));
    CHECK(strcmp(ns.note[1].text, "the wifi password is Blue-Fox-88") == 0 && m.note_dirty == 2);
    CHECK(says(ck_allen_answer(&m, "save the garage codes are 9012", 1), "Saved. It links to 1 other note."));
    CHECK(says(ck_allen_answer(&m, "What's the gate code?", 1), "From your notes: the gate code is 4471."));
    CHECK(m.lit == 1 && m.map_changed == 1);
    CHECK(says(ck_allen_answer(&m, "what is the wifi password", 1),
               "From your notes: the wifi password is Blue-Fox-88."));
    CHECK(says(ck_allen_answer(&m, "find code", 1),
               "From your notes: the garage codes are 9012. I have 1 more on that."));
    CHECK(says(ck_allen_answer(&m, "find the electrician", 1), "I have nothing saved about the electrician."));
    m.changed = m.note_dirty = 0;
    CHECK(says(ck_allen_answer(&m, "save the gate code is 4471.", 1), "I already have that saved."));
    CHECK(ns.n == 3 && m.changed == 0 && m.note_dirty == 0);
    CHECK(says(ck_allen_answer(&m, "how many notes", 1), "You have 3 notes. The newest: the garage codes are 9012."));
    CHECK(says(ck_allen_answer(&m, "show me my notes", 1), "You have 3 notes."));
    CHECK(says(ck_allen_answer(&m, "save", 1), "Tell me what to save"));
    CHECK(says(ck_allen_answer(&m, "help", 1), "keep notes"));
    CHECK(says(ck_allen_answer(&m, "delete the note about the garage", 1),
               "Deleted the note: the garage codes are 9012."));
    CHECK(ns.note[2].gone && m.note_dirty == 3 && ns.n == 3);
    CHECK(strstr(ck_allen_answer(&m, "what's the garage code", 1), "From your notes") == NULL);
    CHECK(says(ck_allen_answer(&m, "delete the note about the boat", 1), "I have no note about the boat."));
    /* taught facts still answer their own questions; machine questions too */
    CHECK(says(ck_allen_answer(&m, "remember my car is red", 1), "Got it: your car is red."));
    CHECK(says(ck_allen_answer(&m, "what is my car", 1), "Your car is red."));
    CHECK(says(ck_allen_answer(&m, "what is your name", 1), "Allen"));
    CHECK(ck_allen_notes_live(&ns) == 2 && ck_allen_note_degree(&ns, 0) == 0);

    /* on disk: three notes, a reload, a damaged unit, a cut write */
    disk_file f;
    disk_dev d;
    uint32_t damaged = 9;
    CHECK(disk_file_open(&f, &d, g_path, 512, (CK_ALLEN_DISK_UNITS + 2) * 8, 1) == 0);
    for (uint32_t i = 0; i < ns.n; i++)
        CHECK(ck_allen_note_write(&d, &ns, i) == CK_ALLEN_MEM_OK);
    CHECK(ck_allen_notes_load(&d, 3, &back, &damaged) == CK_ALLEN_MEM_OK && damaged == 0 && back.n == 3);
    CHECK(strcmp(back.note[1].text, ns.note[1].text) == 0 && back.note[2].gone == 1 && back.note[0].gone == 0);
    CHECK(back.note[0].nword == ns.note[0].nword && back.note[0].word[0] == ns.note[0].word[0]);
    /* a cut while writing note 3 leaves the three saved notes as they were */
    CHECK(ck_allen_note_set(&ns.note[3], "the boat is in slip 12", 1) == 0);
    disk_file_arm_cut(&f, 3);
    CHECK(ck_allen_note_write(&d, &ns, 3) != CK_ALLEN_MEM_OK);
    disk_file_disarm(&f);
    CHECK(ck_allen_notes_load(&d, 3, &back, &damaged) == CK_ALLEN_MEM_OK && damaged == 0);
    /* one spoiled byte: that note loads as gone and is counted */
    disk_file_close(&f);
    FILE *fp = fopen(g_path, "r+b");
    CHECK(fp && fseek(fp, (long)((CK_ALLEN_NOTES_FIRST + 1) * 4096 + 70), SEEK_SET) == 0 && fputc('X', fp) != EOF);
    if (fp)
        fclose(fp);
    CHECK(disk_file_open(&f, &d, g_path, 512, 0, 0) == 0);
    CHECK(ck_allen_notes_load(&d, 3, &back, &damaged) == CK_ALLEN_MEM_OK && damaged == 1 && back.note[1].gone);
    CHECK(ck_allen_notes_load(&d, CK_ALLEN_NOTES_MAX + 1, &back, &damaged) == CK_ALLEN_MEM_EGEOM);
    disk_file_close(&f);
    /* a unit is valid only in its own slot, and only printable text */
    ck_allen_note_encode(&ns.note[0], 0, u);
    CHECK(ck_allen_note_decode(u, 0, &back.note[0]) == 0 && ck_allen_note_decode(u, 1, &back.note[0]) != 0);
    u[64] = 0x07;
    CHECK(ck_allen_note_decode(u, 0, &back.note[0]) != 0);
    CHECK(ck_allen_note_set(&back.note[0], "bad \x01 byte", 1) != 0 && ck_allen_note_set(&back.note[0], "   ", 1) != 0);

    /* the note count travels in the memory copies; a restart says so */
    uint64_t gen = 0;
    m.mem.starts = 1;
    ck_allen_mem_encode(&m.mem, 7, u);
    ck_allen_mind_init(&m2, 0);
    CHECK(ck_allen_mem_decode(u, &m2.mem, &gen) == 0 && m2.mem.nnotes == 3);
    m2.notes = &ns;
    ns.n = 3;
    CHECK(says(ck_allen_welcome(&m2), "Welcome back. I still remember 1 thing you taught me, and your 2 notes."));
    m2.mem.nfacts = 0;
    CHECK(says(ck_allen_welcome(&m2), "Welcome back. I still have your 2 notes."));
    m.mem.nnotes = CK_ALLEN_NOTES_MAX + 1;
    ck_allen_mem_encode(&m.mem, 8, u);
    CHECK(ck_allen_mem_decode(u, &m2.mem, &gen) != 0);
    /* a full notebook says so */
    ck_allen_mind_init(&m, 0);
    memset(&ns, 0, sizeof ns);
    m.notes = &ns;
    ns.n = CK_ALLEN_NOTES_MAX;
    CHECK(says(ck_allen_answer(&m, "save one more thing", 1), "My notebook is full."));
    /* without a disk, notes last until power off and Allen says so */
    ns.n = 0;
    CHECK(says(ck_allen_answer(&m, "save the dentist is on friday", 1), "Only until this computer turns off"));
}

int main(int argc, char **argv)
{
    snprintf(g_path, sizeof g_path, "%s/disk.img", argc > 1 ? argv[1] : ".");
    test_codec();
    test_store();
    test_gpt_add(512);
    test_gpt_add(4096);
    test_teach_restart();
    test_notes();
    printf("allen memory: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
