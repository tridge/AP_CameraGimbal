#include "apcam/APC_Timezone.h"
#include <cassert>
#include <cstdio>

static void check(const char *zone, time_t stamp, long offset)
{
    assert(APC_Timezone::apply(zone) == 0);
    tm local {};
    assert(localtime_r(&stamp, &local));
    assert(local.tm_gmtoff == offset);
    // This is also the inverse conversion used for UniGCS calendar replies.
    local.tm_isdst = -1;
    assert(mktime(&local) == stamp);
    char text[32];
    assert(strftime(text, sizeof(text), "%Z %z", &local));
}

int main(int argc, char **)
{
    assert(!APC_Timezone::valid("Australia/Sydny"));
    assert(!APC_Timezone::valid("Mars/Olympus"));
    assert(!APC_Timezone::valid(":/etc/passwd"));
    assert(!APC_Timezone::valid("/etc/localtime"));
    assert(!APC_Timezone::valid("GMT-foo"));
    assert(!APC_Timezone::valid("GMT-10\n"));
    const struct { const char *name; long january, july; } cases[] = {
        {"GMT", 0, 0}, {"UTC", 0, 0},
        {"Europe/London", 0, 3600},
        {"America/New_York", -18000, -14400},
        {"Australia/Sydney", 39600, 36000},
        {"Australia/Brisbane", 36000, 36000},
        {"Australia/Lord_Howe", 39600, 37800},
        {"Asia/Kathmandu", 20700, 20700},
        {"Pacific/Chatham", 49500, 45900},
        {"Europe/Dublin", 0, 3600},
        {"GMT-10", 36000, 36000}, {"GMT+10", -36000, -36000},
        {"AEST-10AEDT,M10.1.0,M4.1.0/3", 39600, 36000},
    };
    for (const auto &c : cases) {
        check(c.name, 1798761600, c.january); // 2027-01-01 UTC
        check(c.name, 1814400000, c.july); // 2027-07-01 UTC
    }
    check("America/New_York", 1805007599, -18000);
    check("America/New_York", 1805007600, -14400);
    check("Australia/Sydney", 1822492799, 36000);
    check("Australia/Sydney", 1822492800, 39600);
    assert(APC_Timezone::apply("GMT") == 0);
    assert(APC_Timezone::apply("Bad/Zone") == -1 && errno == EINVAL);
    assert(!strcmp(getenv("TZ"), "GMT0"));
    // Optional cross-libc comparison, including DST boundary probes.
    if (argc > 1) {
        for (const auto &zone : APC_Timezone::zones) {
            assert(APC_Timezone::apply(zone.name) == 0);
            for (time_t stamp = 1798761600; stamp < 1830297600; stamp += 3600) {
                tm local {}; assert(localtime_r(&stamp, &local));
                char text[32]; assert(strftime(text, sizeof(text), "%Z %z", &local));
                printf("%s %ld %s\n", zone.name, long(stamp), text);
            }
        }
    }
    puts("PASS named timezones, legacy settings, DST transitions and inverse calendar conversion");
}
