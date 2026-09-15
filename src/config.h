#pragma once

#include <stdbool.h>
#include <stdint.h>

enum afmf_fast_motion_response {
    AFMF_RESPONSE_REPEAT_FRAMES = 0, /* untrusted pixels show the previous frame again */
    AFMF_RESPONSE_BLENDED_FRAMES = 1, /* untrusted pixels blend the two frames without motion */
};

struct afmf_config {
    int log_level;               /* AFMF_LOG, 0..3; default AFMF_LOG_WARN */
    uint32_t extra_images;       /* AFMF_EXTRA_IMAGES, 1..8: swapchain images added for generation */
    uint64_t acquire_timeout_ns; /* AFMF_ACQUIRE_TIMEOUT_US: how long to wait for a free image */
    bool interpolate;            /* AFMF_INTERPOLATE=0 falls back to repeating the previous frame */
    uint32_t flow_levels;        /* AFMF_SEARCH_MODE: optical flow pyramid levels, 5 standard, 7 high */
    enum afmf_fast_motion_response fast_motion; /* AFMF_FAST_MOTION_RESPONSE: repeat | blend */
    const char *dump_dir;        /* AFMF_DUMP_DIR: where the first generated frames are written as PPM */
    bool profile;                /* AFMF_PROFILE=1 (or AFMF_LOG=3): GPU time per pass, logged periodically */
    bool invalid;                /* some variable was set but unparsable; caller reports it */
};

/* Parsed from the environment once, on first use; read-only afterwards. */
const struct afmf_config *afmf_config_get(void);
