#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/backends/mt11/e5739.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/camera-app-e5739-XXXXXX";
    int descriptor = mkstemp(path);
    float factor;
    int zoom;
    int focus;
    int focus_minimum;
    int focus_maximum;
    FILE *file;

    assert(descriptor >= 0);
    file = fdopen(descriptor, "w");
    assert(file != NULL);
    assert(fprintf(file, "1.00 15.32 3.85 31 68 68 67 67 65 62 59 28 23 17\n") > 0);
    assert(fprintf(file, "2.00 30.64 4.00 537 364 364 361 359 354 349 343 14 11 8\n") > 0);
    assert(fprintf(file, "3.20 49.62 4.47 907 737 737 732 727 720 711 702 9 7 5\n") > 0);
    assert(fclose(file) == 0);

    assert(ca_e5739_lookup_calibration(path, 1.0f, &factor, &zoom, &focus) == 0);
    assert(fabsf(factor - 1.0f) < 0.001f && zoom == 31 && focus == 59);
    assert(ca_e5739_lookup_focus_range(path, 1.0f, &focus_minimum,
                                       &focus_maximum) == 0);
    assert(focus_minimum == 59 && focus_maximum == 68);
    assert(ca_e5739_lookup_calibration(path, 1.1f, &factor, &zoom, &focus) == 0);
    assert(fabsf(factor - 2.0f) < 0.001f && zoom == 537 && focus == 343);
    assert(ca_e5739_lookup_calibration(path, 9.0f, &factor, &zoom, &focus) == 0);
    assert(fabsf(factor - 3.2f) < 0.001f && zoom == 907 && focus == 702);
    assert(ca_e5739_lookup_focus_range(path, 9.0f, &focus_minimum,
                                       &focus_maximum) == 0);
    assert(focus_minimum == 702 && focus_maximum == 737);
    assert(!ca_e5739_system_uses_tele(3.44f));
    assert(ca_e5739_system_uses_tele(3.45f));
    assert(fabsf(ca_e5739_system_to_optical(2.0f) - 1.0f) < 0.001f);
    assert(fabsf(ca_e5739_system_to_optical(4.0f) - 1.1f) < 0.001f);
    assert(fabsf(ca_e5739_system_to_optical(10.0f) - 2.9f) < 0.001f);
    assert(unlink(path) == 0);
    puts("PASS E5739 calibration lookup");
    return 0;
}
