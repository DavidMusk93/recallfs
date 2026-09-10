#ifndef ASYNC_EXAMPLES_H
#define ASYNC_EXAMPLES_H

#include <stdbool.h>

typedef struct aa_observation {
    char trace[32];
    unsigned task_polls;
    bool constructor_was_inert;
    bool completed;
    bool cancelled;
    bool detached;
    bool cleanup_ran;
    bool wakeups_coalesced;
} aa_observation;

bool aa_observe_lazy(aa_observation *observation);
bool aa_observe_dynamic_await(aa_observation *observation);
bool aa_observe_wake(aa_observation *observation);
bool aa_observe_detach(aa_observation *observation);
bool aa_observe_cancel(aa_observation *observation);

#endif
