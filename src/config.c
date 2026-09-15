#include "config.h"

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Whether this process is Gamescope itself. With AFMF_GAMESCOPE=1 the same environment reaches
 * the compositor and the game it runs, and only the game should get companions. */
static bool process_is_gamescope(void)
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    const char *base = strrchr(exe, '/');
    return strcmp(base != NULL ? base + 1 : exe, "gamescope") == 0;
}

/* Runs under pthread_once: it must not log, because logging reads the config back. */
static void init(void)
{
    long log_level = AFMF_LOG_WARN;
    long gamescope = 0;
    bool ok = read_bounded("AFMF_GAMESCOPE", 0, 1, &gamescope);
    /* Measured with vkcube on a 165 Hz Wayland desktop: a free image only comes back once the
     * compositor releases one, about one refresh period after a present. Two extra images make
     * one available by the next present; one extra was not enough. Gamescope keeps more images
     * in flight (direct scanout): with 2 extra a third of the presents found a free one, with 4
     * most, with 5 all of them, and 3 + 5 stays within the 8 some engines accept. The companion's
     * image is acquired a frame ahead (swapchain.c, spare_*), so no wait is needed in the present
     * hook: the timeout only bounds the wait for a spare's release, and 0 means never stall. */
    long extra_images = gamescope != 0 ? 5 : 2;
    long acquire_timeout_us = 0;
    long interpolate = 1;
    /* ADLX search mode: standard keeps the search to 5 pyramid levels (+-128 px), high uses all 7
     * (+-512 px); auto lets the flow resolution decide (framegen.c). */
    long search_mode = AFMF_SEARCH_AUTO;
    long fast_motion = AFMF_RESPONSE_REPEAT_FRAMES;
    long performance = AFMF_PERFORMANCE_AUTO;
    long profile = 0;
    long async = 1;
    /* Pacing holds the real frame back by half a frame time so the generated one lands halfway,
     * as the driver-level implementation does (its documented 4-5 ms of added latency at 120 fps). */
    long pacing = 1;
    /* Every present takes a vblank and the layer doubles the presents: in FIFO a game above half
     * the refresh rate loses real frames (120 at 165 Hz -> 82). MAILBOX drops the excess instead. */
    long present_mode = AFMF_PRESENT_AUTO;
    /* Under GPU contention the generated frame is ready late and the layer's work is taken
     * from the game's budget: the governor steps generation down (fewer search levels, then one
     * companion in two, then in three) and back up as the GPU catches up. Below the minimum real
     * frame rate doubling is not worth its latency. */
    long governor = 1;
    long min_fps = 30;
    /* Sum of absolute 8-bit luma differences over a block's 64 pixels between the two frames at
     * rest; 128 is two levels per pixel on average, what temporal anti-aliasing leaves on a
     * still image. Under it the block is stored as static and its search is skipped. */
    long static_block_sad = 128;

    static const struct choice search_modes[] = {{"auto", AFMF_SEARCH_AUTO},
                                                {"standard", AFMF_SEARCH_STANDARD},
                                                {"high", AFMF_SEARCH_HIGH}};
    static const struct choice responses[] = {{"repeat", AFMF_RESPONSE_REPEAT_FRAMES},
                                             {"blend", AFMF_RESPONSE_BLENDED_FRAMES}};
    static const struct choice performance_modes[] = {{"auto", AFMF_PERFORMANCE_AUTO},
                                                     {"quality", AFMF_PERFORMANCE_QUALITY},
                                                     {"performance", AFMF_PERFORMANCE_FAST}};
    static const struct choice present_modes[] = {{"auto", AFMF_PRESENT_AUTO},
                                                 {"keep", AFMF_PRESENT_KEEP}};

    ok = read_bounded("AFMF_LOG", AFMF_LOG_ERROR, AFMF_LOG_DEBUG, &log_level) && ok;
    ok = read_bounded("AFMF_EXTRA_IMAGES", 1, 8, &extra_images) && ok;
    ok = read_bounded("AFMF_ACQUIRE_TIMEOUT_US", 0, 100000, &acquire_timeout_us) && ok;
    ok = read_bounded("AFMF_INTERPOLATE", 0, 1, &interpolate) && ok;
    ok = read_choice("AFMF_SEARCH_MODE", search_modes, 3, &search_mode) && ok;
    ok = read_choice("AFMF_FAST_MOTION_RESPONSE", responses, 2, &fast_motion) && ok;
    ok = read_bounded("AFMF_PROFILE", 0, 1, &profile) && ok;
    ok = read_bounded("AFMF_ASYNC", 0, 1, &async) && ok;
    ok = read_bounded("AFMF_PACING", 0, 1, &pacing) && ok;
    ok = read_choice("AFMF_PERFORMANCE_MODE", performance_modes, 3, &performance) && ok;
    ok = read_choice("AFMF_PRESENT_MODE", present_modes, 2, &present_mode) && ok;
    ok = read_bounded("AFMF_GOVERNOR", 0, 1, &governor) && ok;
    ok = read_bounded("AFMF_MIN_FPS", 0, 240, &min_fps) && ok;
    ok = read_bounded("AFMF_STATIC_BLOCK_SAD", 0, 16320, &static_block_sad) && ok;

    g_config.log_level = (int)log_level;
    g_config.extra_images = (uint32_t)extra_images;
    g_config.acquire_timeout_ns = (uint64_t)acquire_timeout_us * 1000u;
    g_config.interpolate = interpolate != 0;
    g_config.search_mode = (enum afmf_search_mode)search_mode;
    g_config.flow_levels = search_mode == AFMF_SEARCH_STANDARD ? 5u : 7u;
    g_config.fast_motion = (enum afmf_fast_motion_response)fast_motion;
    g_config.performance = (enum afmf_performance_mode)performance;
    const char *dump_dir = getenv("AFMF_DUMP_DIR");
    g_config.dump_dir = dump_dir != NULL && *dump_dir != '\0' ? dump_dir : NULL;
    g_config.profile = profile != 0 || log_level >= AFMF_LOG_DEBUG;
    g_config.async = async != 0;
    g_config.pacing = pacing != 0;
    g_config.passive = gamescope != 0 && process_is_gamescope();
    g_config.present_mode = (enum afmf_present_mode)present_mode;
    g_config.governor = governor != 0;
    g_config.min_fps = (uint32_t)min_fps;
    g_config.static_block_sad = (uint32_t)static_block_sad;
    g_config.invalid = !ok;
}

const struct afmf_config *afmf_config_get(void)
{
    (void)pthread_once(&g_once, init); /* only fails on an invalid once_control, which this is not */
    return &g_config;
}
