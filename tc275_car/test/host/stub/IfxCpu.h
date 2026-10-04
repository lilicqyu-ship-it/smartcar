/*
 * IfxCpu.h - host stand-in for the iLLD CPU sync/mutex API
 *
 * Only what mw/xcore/xcore.c compiles against on the host: the cross-core
 * spinlock type and its acquire/release pair. The host test is single
 * threaded, so the lock is vacuous - what the tests cover is the queue and
 * ring discipline between the calls, not the atomicity the hardware lock
 * provides on the target. Lives on the include path of test/host so the
 * production sources stay untouched; the TriCore build never sees this file.
 */
#ifndef HOST_IFXCPU_H
#define HOST_IFXCPU_H

#include <stdint.h>
#include <assert.h>

#include "Ifx_Types.h"

typedef volatile uint32 IfxCpu_mutexLock;
static boolean hostInterrupts = TRUE;
static inline boolean IfxCpu_disableInterrupts(void) {
    boolean previous=hostInterrupts; hostInterrupts=FALSE; return previous;
}
static inline void IfxCpu_restoreInterrupts(boolean enabled) { hostInterrupts=enabled; }
static inline uint32 IfxCpu_getCoreId(void) { return 0; }

static inline boolean IfxCpu_acquireMutex(IfxCpu_mutexLock *lock)
{
    assert(hostInterrupts == FALSE);
    (void)lock;
    return TRUE;
}

static inline void IfxCpu_releaseMutex(IfxCpu_mutexLock *lock)
{
    assert(hostInterrupts == FALSE);
    (void)lock;
}

/* TriCore core-synchronisation intrinsic; nothing to order on the host. */
#define __dsync() ((void)0)

#endif /* HOST_IFXCPU_H */
