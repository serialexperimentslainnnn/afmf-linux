#include "config.h"

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

static struct afmf_config g_config;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* Parses a decimal integer in [min, max]; returns false on any other input (including garbage after
 * the number, overflow, or an empty string). */
static bool parse_bounded(const char *text, long min, long max, long *out)
{
    if (text == NULL || *text == '\0')
        return false;

    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < min || value > max)
        return false;

    *out = value;
    return true;
}

/* Runs under pthread_once: it must not log, because logging reads the config back. */
static void init(void)
{
    g_config.log_level = AFMF_LOG_WARN;

    const char *text = getenv("AFMF_LOG");
    if (text == NULL)
        return;

    long level;
    if (parse_bounded(text, AFMF_LOG_ERROR, AFMF_LOG_DEBUG, &level))
        g_config.log_level = (int)level;
    else
        g_config.log_level_invalid = true;
}

const struct afmf_config *afmf_config_get(void)
{
    (void)pthread_once(&g_once, init); /* only fails on an invalid once_control, which this is not */
    return &g_config;
}
