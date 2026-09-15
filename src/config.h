#pragma once

#include <stdbool.h>
#include <stdint.h>

enum afmf_performance_mode {
    AFMF_PERFORMANCE_AUTO = 0,    /* half-resolution flow from 2560x1440 upwards */
    AFMF_PERFORMANCE_QUALITY = 1, /* optical flow at display resolution */
    AFMF_PERFORMANCE_FAST = 2,    /* optical flow at half resolution, 16-pixel blocks on screen */
};

enum afmf_search_mode {
    AFMF_SEARCH_AUTO = 0,     /* 7 levels at full flow resolution, 5 at reduced */
    AFMF_SEARCH_STANDARD = 1, /* 5 levels */
    AFMF_SEARCH_HIGH = 2,     /* 7 levels always */
};

enum afmf_fast_motion_response {
    AFMF_RESPONSE_REPEAT_FRAMES = 0, /* untrusted pixels show the previous frame again */
    AFMF_RESPONSE_BLENDED_FRAMES = 1, /* untrusted pixels blend the two frames without motion */
};

struct afmf_config {
    int log_level;               /* AFMF_LOG, 0..3; default AFMF_LOG_WARN */
    uint32_t extra_images;       /* AFMF_EXTRA_IMAGES, 1..8: swapchain images added for generation */
    uint64_t acquire_timeout_ns; /* AFMF_ACQUIRE_TIMEOUT_US: how long to wait for the spare's release */
    bool interpolate;            /* AFMF_INTERPOLATE=0 falls back to repeating the previous frame */
    uint32_t flow_levels;        /* optical flow pyramid levels derived from search_mode: 5 or 7 */
    enum afmf_search_mode search_mode; /* AFMF_SEARCH_MODE: auto | standard | high */
    enum afmf_fast_motion_response fast_motion; /* AFMF_FAST_MOTION_RESPONSE: repeat | blend */
    enum afmf_performance_mode performance;     /* AFMF_PERFORMANCE_MODE: auto | quality | performance */
    const char *dump_dir;        /* AFMF_DUMP_DIR: where the first generated frames are written as PPM */
    bool profile;                /* AFMF_PROFILE=1 (or AFMF_LOG=3): GPU time per pass, logged periodically */
    bool async;                  /* AFMF_ASYNC=0 keeps the work on the application's queue */
    bool pacing;                 /* AFMF_PACING=0 presents the real frame right behind the generated one */
    bool passive;                /* AFMF_GAMESCOPE=1 and this process is Gamescope itself: pass-through */
    bool invalid;                /* some variable was set but unparsable; caller reports it */
};

/* Parsed from the environment once, on first use; read-only afterwards. */
const struct afmf_config *afmf_config_get(void);
