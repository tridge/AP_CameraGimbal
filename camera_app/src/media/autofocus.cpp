#include "camera_app/autofocus.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

static bool better(uint64_t score, int position, uint64_t best_score,
                   int best_position, int initial, bool have_best)
{
    int64_t distance;
    int64_t best_distance;

    if (!have_best || score > best_score) return true;
    if (score < best_score) return false;
    distance = (int64_t)position - initial;
    best_distance = (int64_t)best_position - initial;
    if (distance < 0) distance = -distance;
    if (best_distance < 0) best_distance = -best_distance;
    return distance < best_distance;
}

static int sample_position(const struct ca_autofocus_ops *ops, int position,
                           int initial, uint64_t *best_score,
                           int *best_position, bool *have_best,
                           unsigned *samples)
{
    uint64_t score;

    if (ops->move(ops->opaque, position) < 0 ||
        ops->measure(ops->opaque, &score) < 0) return -1;
    (*samples)++;
    if (better(score, position, *best_score, *best_position, initial,
               *have_best)) {
        *best_score = score;
        *best_position = position;
        *have_best = true;
    }
    return 0;
}

int ca_autofocus_search(int minimum, int maximum, int initial,
                        unsigned coarse_step,
                        const struct ca_autofocus_ops *ops,
                        struct ca_autofocus_result *result)
{
    uint64_t best_score = 0;
    uint64_t span;
    int best_position = initial;
    bool have_best = false;
    unsigned samples = 0;
    int refine_min;
    int refine_max;

    span = (uint64_t)((int64_t)maximum - minimum) + 1U;
    if (minimum > maximum || initial < minimum || initial > maximum ||
        coarse_step == 0U || coarse_step > span || coarse_step > INT_MAX ||
        ops == NULL || ops->move == NULL || ops->measure == NULL ||
        result == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (int position = minimum;;) {
        if (sample_position(ops, position, initial, &best_score,
                            &best_position, &have_best, &samples) < 0) {
            goto fail;
        }
        if (position == maximum) break;
        if (maximum - position <= (int)coarse_step) position = maximum;
        else position += (int)coarse_step;
    }
    refine_min = best_position - (int)coarse_step;
    refine_max = best_position + (int)coarse_step;
    if (refine_min < minimum) refine_min = minimum;
    if (refine_max > maximum) refine_max = maximum;
    /* The fine pass is authoritative; do not retain a noisy coarse sample. */
    have_best = false;
    for (int position = refine_min; position <= refine_max; position++) {
        if (sample_position(ops, position, initial, &best_score,
                            &best_position, &have_best, &samples) < 0) {
            goto fail;
        }
    }
    if (ops->move(ops->opaque, best_position) < 0) goto fail;
    result->position = best_position;
    result->score = best_score;
    result->samples = samples;
    return 0;

fail:
    {
        int saved_errno = errno;
        (void)ops->move(ops->opaque, initial);
        errno = saved_errno;
    }
    return -1;
}
