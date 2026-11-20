#include "camera_app/autofocus.h"

#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct fixture {
    int position;
    int peak;
    int fail_position;
    int bonus_position;
    bool bonus_used;
};

static int move_focus(void *opaque, int position)
{
    struct fixture *fixture = (struct fixture*)(opaque);
    fixture->position = position;
    return 0;
}

static int measure_focus(void *opaque, uint64_t *score)
{
    struct fixture *fixture = (struct fixture*)(opaque);
    int distance;
    if (fixture->position == fixture->fail_position) {
        errno = EIO;
        return -1;
    }
    distance = abs(fixture->position - fixture->peak);
    *score = (uint64_t)(10000 - distance * distance);
    if (fixture->position == fixture->bonus_position &&
        !fixture->bonus_used) {
        *score += 100U;
        fixture->bonus_used = true;
    }
    return 0;
}

int main(void)
{
    struct fixture fixture = {.position = 62, .peak = 67,
                              .fail_position = -1, .bonus_position = -1};
    struct ca_autofocus_ops ops = {
        .move = move_focus,
        .measure = measure_focus,
        .opaque = &fixture,
    };
    struct ca_autofocus_result result;

    assert(ca_autofocus_search(59, 68, 62, 3, &ops, &result) == 0);
    assert(result.position == 67);
    assert(result.score == 10000U);
    assert(result.samples >= 7U);
    assert(fixture.position == 67);

    fixture.position = 62;
    fixture.peak = 60;
    fixture.bonus_position = 59;
    fixture.bonus_used = false;
    assert(ca_autofocus_search(59, 68, 62, 3, &ops, &result) == 0);
    assert(result.position == 60);

    fixture.position = 62;
    fixture.bonus_position = -1;
    fixture.fail_position = 65;
    errno = 0;
    assert(ca_autofocus_search(59, 68, 62, 3, &ops, &result) < 0);
    assert(errno == EIO);
    assert(fixture.position == 62);

    puts("autofocus search tests passed");
    return 0;
}
