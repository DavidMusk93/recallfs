#include "async_examples.h"

#include <stdbool.h>
#include <stdio.h>

typedef bool (*scenario_fn)(aa_observation *observation);

static const char *yes_no(bool value) {
    return value ? "yes" : "no";
}

static bool run_scenario(const char *name, scenario_fn scenario) {
    aa_observation observation;

    if (!scenario(&observation)) {
        fprintf(stderr, "%s: runtime error\n", name);
        return false;
    }

    printf("%-13s trace=%-2s polls=%u inert=%s complete=%s cancelled=%s "
           "handle-dropped=%s cleanup=%s coalesced=%s\n",
           name, observation.trace, observation.task_polls, yes_no(observation.constructor_was_inert),
           yes_no(observation.completed), yes_no(observation.cancelled), yes_no(observation.handle_dropped),
           yes_no(observation.cleanup_ran), yes_no(observation.wakeups_coalesced));
    return true;
}

int main(void) {
    bool ok = true;

    ok = run_scenario("lazy", aa_observe_lazy) && ok;
    ok = run_scenario("dynamic-await", aa_observe_dynamic_await) && ok;
    ok = run_scenario("wake", aa_observe_wake) && ok;
    ok = run_scenario("handle-drop", aa_observe_handle_drop) && ok;
    ok = run_scenario("cancel", aa_observe_cancel) && ok;
    return ok ? 0 : 1;
}
