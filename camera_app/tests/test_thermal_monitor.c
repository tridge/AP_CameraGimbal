#define _GNU_SOURCE
#include "camera_app/media_impl.h"
#include "camera_app/binlog.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

struct ca_media_impl { int unused; };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool release_io, in_io, closed;
static unsigned exposure_count;
bool ca_binlog_active(void) { return true; }
int ca_media_impl_exposure(struct ca_media_impl *m, unsigned lens, struct ca_exposure *s)
{ (void)m; (void)lens; s->shutter_us=10000; s->valid=CA_AE_SHUTTER; return 0; }

int ca_media_impl_open(struct ca_media_impl **out, const struct ca_media_config *config)
{ (void)config; *out = calloc(1, sizeof(**out)); return *out ? 0 : -1; }
void ca_media_impl_close(struct ca_media_impl *media)
{
    pthread_mutex_lock(&lock);
    assert(!in_io);
    closed = true;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
    free(media);
}
int ca_media_impl_get_thermal_gain(struct ca_media_impl *media, uint8_t *gain)
{
    (void)media;
    pthread_mutex_lock(&lock);
    assert(!closed);
    in_io = true;
    pthread_cond_broadcast(&wake);
    while (!release_io) pthread_cond_wait(&wake, &lock);
    in_io = false;
    pthread_mutex_unlock(&lock);
    *gain = 1;
    return 0;
}
int ca_media_impl_get_thermal_palette(struct ca_media_impl *media, uint8_t *palette)
{ (void)media; *palette = 0; return 0; }
bool ca_media_impl_recording(const struct ca_media_impl *media)
{ (void)media; return false; }
int ca_media_impl_set_recording(struct ca_media_impl *media, bool active)
{ (void)media; (void)active; return 0; }
const char *ca_media_impl_recording_path(const struct ca_media_impl *media)
{ (void)media; return ""; }
void ca_log(const char *format, ...) { (void)format; }
uint64_t ca_binlog_time_us(void) { return 0; }
void ca_binlog_emit(uint8_t id, const void *data, size_t size)
{
    if (id==CA_LOG_AE) {
        assert(size==sizeof(struct ca_exposure));
        assert(((const struct ca_exposure *)data)->shutter_us==10000);
        pthread_mutex_lock(&lock); exposure_count++; pthread_cond_broadcast(&wake); pthread_mutex_unlock(&lock);
    }
}
static void *close_media(void *opaque) { ca_media_close(opaque); return NULL; }

int ca_media_impl_apply_overlay(struct ca_media_impl *m, const struct ca_config *s)
{ (void)m; (void)s; return 0; }

int main(void)
{
    struct ca_media *media;
    struct ca_media_config config = {0};
    assert(ca_media_open(&media, &config) == 0);
    pthread_mutex_lock(&lock);
    while (!in_io) pthread_cond_wait(&wake, &lock);
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&lock);
    while (exposure_count<6) pthread_cond_wait(&wake,&lock);
    pthread_mutex_unlock(&lock);
    /* A permanently blocked sensor read must not block cache consumers. The
     * test runner times out if a cache call accidentally holds an I/O lock. */
    uint8_t gain, palette;
    for (unsigned i = 0; i < 10000; i++)
        assert(!ca_media_cached_thermal_controls(media, &gain, &palette));
    pthread_mutex_lock(&lock);
    release_io = true;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < 1000; i++) {
        if (ca_media_cached_thermal_controls(media, &gain, &palette)) break;
        usleep(1000);
    }
    assert(ca_media_cached_thermal_controls(media, &gain, &palette));
    assert(gain == 1 && palette == 0);
    pthread_mutex_lock(&lock);
    release_io = false;
    while (!in_io) pthread_cond_wait(&wake, &lock);
    pthread_mutex_unlock(&lock);
    pthread_t closer;
    assert(pthread_create(&closer, NULL, close_media, media) == 0);
    usleep(20000);
    pthread_mutex_lock(&lock);
    assert(!closed); /* Shutdown must join the blocked poll before closing. */
    release_io = true;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
    pthread_join(closer, NULL);
    assert(closed);
    puts("PASS blocked thermal polling leaves the control cache responsive and joins before close");
    return 0;
}
