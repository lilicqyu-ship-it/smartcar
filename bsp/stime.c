#include "bsp/stime.h"
#include "IfxStm.h"

static uint32 g_ticksPerMs;

void STIME_init(void)
{
    g_ticksPerMs = (uint32)(IfxStm_getFrequency(&MODULE_STM0) / 1000.0f);
}

uint32 STIME_nowMs(void)
{
    return (uint32)(IfxStm_get(&MODULE_STM0) / g_ticksPerMs);
}

void STIME_waitUntilMs(uint32 ms)
{
    while ((sint32)(STIME_nowMs() - ms) < 0)
    {
    }
}

void STIME_delayMs(uint32 ms)
{
    uint32 start = STIME_nowMs();

    while ((STIME_nowMs() - start) < ms)
    {
    }
}
