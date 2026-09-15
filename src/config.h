#pragma once

#include <stdbool.h>

struct afmf_config {
    int log_level;          /* AFMF_LOG, 0..3; default AFMF_LOG_WARN */
    bool log_level_invalid; /* AFMF_LOG was set but unparsable; caller reports it */
};

/* Parsed from the environment once, on first use; read-only afterwards. */
const struct afmf_config *afmf_config_get(void);
