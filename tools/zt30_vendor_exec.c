/* Restore captured supplementary groups before re-executing the ZT30 vendor.
 * Groups are the only missing inherited property: SSH initgroups adds group0,
 * whereas init's vendor launch has no supplementary groups. No SDK calls. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef ZT30_VENDOR_APP
#define ZT30_VENDOR_APP "/customer/siyi_app_359"
#endif

int main(int argc, char **argv)
{
    gid_t groups[64], current[64];
    const char *text;
    int count = 0, existing;
    int check = argc == 3 && strcmp(argv[1], "--check-groups") == 0;
    if (argc < 3 || (!check && strcmp(argv[2], ZT30_VENDOR_APP) != 0)) {
        fputs("usage: vendor-exec GROUPS vendor-program [ARGS...]\n", stderr);
        return 2;
    }
    text = check ? argv[2] : argv[1];
    while (*text) {
        char *end;
        unsigned long value;
        while (*text == ' ' || *text == '\t') ++text;
        if (!*text) break;
        if (*text < '0' || *text > '9' || count == 64) return 2;
        errno = 0;
        value = strtoul(text, &end, 10);
        if (errno || value > UINT_MAX || (unsigned long)(gid_t)value != value ||
            (*end && *end != ' ' && *end != '\t')) return 2;
        groups[count++] = (gid_t)value;
        text = end;
    }
    existing = getgroups(64, current);
    if (existing < 0) { perror("getgroups"); return 3; }
    /* Unprivileged host fixtures can keep an already identical group list. */
    if ((existing != count || memcmp(groups, current, count * sizeof(gid_t)) != 0) &&
        setgroups((size_t)count, groups) != 0) {
        perror("setgroups");
        return 3;
    }
    if (check) { printf("group_count=%d\n", getgroups(0, NULL)); return 0; }
    execv(argv[2], argv + 2);
    perror("execv vendor");
    return 4;
}
