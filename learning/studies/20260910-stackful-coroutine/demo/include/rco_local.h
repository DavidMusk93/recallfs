#ifndef RCO_LOCAL_H
#define RCO_LOCAL_H

#if !defined(_GNU_SOURCE)
#error "rco_local.h requires _GNU_SOURCE before any system header"
#endif

#include "rco.h"

#include <locale.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Signal masks and locales are task-only APIs and require their corresponding
 * local-state flags. rco_locale_set() duplicates its input, and
 * rco_locale_get() returns a borrowed task-owned handle. When deferred
 * preemption is enabled, config.preempt_signal is reserved by the runtime;
 * rco_sigmask() rejects SIG_BLOCK and SIG_SETMASK sets containing it with
 * -EINVAL.
 */
int rco_sigmask(int how, const sigset_t *set, sigset_t *old_set);
int rco_locale_set(locale_t locale);
int rco_locale_get(locale_t *out_locale);

#ifdef __cplusplus
}
#endif

#endif
