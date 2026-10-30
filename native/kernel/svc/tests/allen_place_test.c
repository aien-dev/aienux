/* allen_place_test.c -- host test of ck_allen_parse_place (native/kernel/svc/allen_reply.c):
 * coordinates typed to Allen, in the forms Drake will use, and the lines that must not
 * be taken for a place. Prints one line per check and ends with "ALLEN_PLACE: PASS" or
 * "ALLEN_PLACE: FAIL <n> checks". */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "allen_reply.h"

static int fails = 0, checks = 0;

static void expect(const char *line, int want, int32_t lat, int32_t lon)
{
    int32_t la = 7, lo = 7;
    int got = ck_allen_parse_place(line, &la, &lo);
    int ok = got == want && (!want || (la == lat && lo == lon));
    checks++;
    if (!ok)
        fails++;
    printf("%s  \"%s\" -> %d (%d.%06d, %d.%06d) want %d (%d, %d)\n", ok ? "ok    " : "FAIL  ", line, got,
           la / 1000000, la % 1000000 < 0 ? -(la % 1000000) : la % 1000000, lo / 1000000,
           lo % 1000000 < 0 ? -(lo % 1000000) : lo % 1000000, want, lat, lon);
}

int main(void)
{
    /* Drake's forms, and the ones that should match too */
    expect("35 north 90 west", 1, 35000000, -90000000);
    expect("35 North 90 West", 1, 35000000, -90000000);
    expect("35.1 n, 90.0 w", 1, 35100000, -90000000);
    expect("35.1, -90.0", 1, 35100000, -90000000);
    expect("35.1,-90.0", 1, 35100000, -90000000);
    expect("I'm at 35.1, -90.0.", 1, 35100000, -90000000);
    expect("location is 35 north, 90 west", 1, 35000000, -90000000);
    expect("lat 35.1 lon -90", 1, 35100000, -90000000);
    expect("-33.87 151.21", 1, -33870000, 151210000); /* a minus sign is enough */
    expect("-33.87, 151.21", 1, -33870000, 151210000);
    expect("33.87 south, 151.21 east", 1, -33870000, 151210000);
    expect("north 35 west 90", 1, 35000000, -90000000);
    expect("35.123456789, -90.5", 1, 35123456, -90500000);
    expect("0 north 0 east", 1, 0, 0);
    /* must not be taken for a place */
    expect("35 90", 0, 0, 0);
    expect("what is 35 - 90", 0, 0, 0);
    expect("what is 35 plus 90", 0, 0, 0);
    expect("35", 0, 0, 0);
    expect("sky above me", 0, 0, 0);
    expect("i'm in memphis", 0, 0, 0);
    expect("my name is 35", 0, 0, 0);
    expect("91 north 10 east", 0, 0, 0); /* latitude out of range */
    expect("10 north 181 east", 0, 0, 0); /* longitude out of range */
    expect("35 north 90 west 4", 0, 0, 0); /* three numbers */
    expect("-35 north, 90 west", 0, 0, 0); /* a sign and a direction on one number */
    expect("", 0, 0, 0);
    printf("ALLEN_PLACE: %s (%d checks, %d failed)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
