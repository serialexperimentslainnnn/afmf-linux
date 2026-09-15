#include "config.h"

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static struct afmf_config g_config;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* Reads an integer variable in [min, max] into *out; leaves it untouched when unset. Returns false
 * only when the variable is set to something unparsable or out of range. */
static bool read_bounded(const char *name, long min, long max, long *out)
{
    const char *text = getenv(name);
    if (text == NULL)
        return true;
    if (*text == '\0')
        return false;

    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < min || value > max)
        return false;

    *out = value;
    return true;
}

struct choice {
    const char *word;
    long value;
};

/* Reads a word variable against a table of choices; same contract as read_bounded. */
static bool read_choice(const char *name, const struct choice *choices, size_t count, long *out)
{
    const char *text = getenv(name);
    if (text == NULL)
        return true;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(text, choices[i].word) == 0) {
            *out = choices[i].value;
            return true;
        }
    }
    return false;
}

/* Runs under pthread_once: it must not log, because logging reads the config back. */
static void init(void)
{
    long log_level = AFMF_LOG_WARN;
    /* Measured with vkcube on a 165 Hz Wayland desktop: a free image only comes back once the
     * compositor releases one, i.e. about one refresh period later, so the wait has to cover a
     * period of the slowest common display (60 Hz, 16.7 ms). Two extra images made 294 of 296
     * presents generate; one extra was not enough. */
    long extra_images = 2;
    long acquire_timeout_us = 16000;
    long interpolate = 1;
    /* ADLX search mode: standard keeps the search to 5 pyramid levels (+-128 px), high uses all 7
     * (+-512 px); auto picks high. */
    long flow_levels = 7;
    long fast_motion = AFMF_RESPONSE_REPEAT_FRAMES;

    static const struct choice search_modes[] = {{"auto", 7}, {"standard", 5}, {"high", 7}};
    static const struct choice responses[] = {{"repeat", AFMF_RESPONSE_REPEAT_FRAMES},
                                             {"blend", AFMF_RESPONSE_BLENDED_FRAMES}};

    bool ok = read_bounded("AFMF_LOG", AFMF_LOG_ERROR, AFMF_LOG_DEBUG, &log_level);
    ok = read_bounded("AFMF_EXTRA_IMAGES", 1, 8, &extra_images) && ok;
    ok = read_bounded("AFMF_ACQUIRE_TIMEOUT_US", 0, 100000, &acquire_timeout_us) && ok;
    ok = read_bounded("AFMF_INTERPOLATE", 0, 1, &interpolate) && ok;
    ok = read_choice("AFMF_SEARCH_MODE", search_modes, 3, &flow_levels) && ok;
    ok = read_choice("AFMF_FAST_MOTION_RESPONSE", responses, 2, &fast_motion) && ok;

    g_config.log_level = (int)log_level;
    g_config.extra_images = (uint32_t)extra_images;
    g_config.acquire_timeout_ns = (uint64_t)acquire_timeout_us * 1000u;
    g_config.interpolate = interpolate != 0;
    g_config.flow_levels = (uint32_t)flow_levels;
    g_config.fast_motion = (enum afmf_fast_motion_response)fast_motion;
    const char *dump_dir = getenv("AFMF_DUMP_DIR");
    g_config.dump_dir = dump_dir != NULL && *dump_dir != '\0' ? dump_dir : NULL;
    g_config.invalid = !ok;
}

const struct afmf_config *afmf_config_get(void)
{
    (void)pthread_once(&g_once, init); /* only fails on an invalid once_control, which this is not */
    return &g_config;
}
