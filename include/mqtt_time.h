#ifndef MQTT_TIME_H
#define MQTT_TIME_H

#include <stdint.h>
#include <time.h>
#include "os_port.h"

/* Monotonic milliseconds for bounded MQTT delivery deadlines. Subtract as
 * uint32_t to tolerate wrapping; the POSIX osGetSystemTime uses wall time. */
static inline bool_t mqtt_monotonic_ms(uint32_t *now)
{
#ifdef _WIN32
    *now = (uint32_t)osGetSystemTime();
    return TRUE;
#else
    struct timespec sample;
    if (clock_gettime(CLOCK_MONOTONIC, &sample) != 0)
        return FALSE;
    *now = (uint32_t)((uint64_t)sample.tv_sec * 1000U +
                     (uint64_t)sample.tv_nsec / 1000000U);
    return TRUE;
#endif
}

#endif
