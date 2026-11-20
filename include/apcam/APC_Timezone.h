#pragma once

#include "timezones.h"
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

// Keep IANA names in configuration; libc receives portable recurring rules.
// This also works on cameras without zoneinfo files (including uClibc ZR10).
namespace APC_Timezone {
inline const char *rule(const char *name)
{
    for (const auto &zone : zones) {
        if (!strcmp(name, zone.name)) return zone.rule;
    }
    return nullptr;
}

inline bool valid(const char *name)
{
    if (rule(name)) return true;
    // Preserve saved POSIX TZ values during upgrades. Require an explicit
    // numeric standard offset so a misspelled IANA name cannot become GMT.
    const char *p = name;
    if (*p == '<') {
        const char *start = ++p;
        while (isalnum(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-') ++p;
        if (p - start < 3 || *p++ != '>') return false;
    } else {
        while (isalpha(static_cast<unsigned char>(*p))) ++p;
        if (p - name < 3) return false;
    }
    if (*p == '+' || *p == '-') ++p;
    if (!isdigit(static_cast<unsigned char>(*p))) return false;
    for (; *p; ++p) {
        if (!isalnum(static_cast<unsigned char>(*p)) && !strchr("+-:,./<>", *p)) return false;
    }
    return true;
}

inline int apply(const char *name)
{
    if (!valid(name)) { errno = EINVAL; return -1; }
    const char *value = rule(name);
    if (!value) value = name;
    const char *current = getenv("TZ");
    if (!current || strcmp(current, value)) {
        if (setenv("TZ", value, 1) < 0) return -1;
        tzset();
    }
    return 0;
}
}
