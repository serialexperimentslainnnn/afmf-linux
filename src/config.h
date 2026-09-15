#pragma once

#include <stdbool.h>
#include <stdint.h>

struct afmf_config {
    int log_level;               /* AFMF_LOG, 0..3; default AFMF_LOG_WARN */
    uint32_t extra_images;       /* AFMF_EXTRA_IMAGES, 1..8: swapchain images added for generation */
    uint64_t acquire_timeout_ns; /* AFMF_ACQUIRE_TIMEOUT_US: how long to wait for a free image */
    bool invalid;                /* some variable was set but unparsable; caller reports it */
};

/* Parsed from the environment once, on first use; read-only afterwards. */
const struct afmf_config *afmf_config_get(void);
