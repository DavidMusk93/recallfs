#include "rco.h"

#if !defined(RCO_NORMAL_CALLER_LINK_PROBE)
#include "rco_internal.h"
#endif

#if !defined(RCO_CACS) && !defined(RCO_NORMAL_CALLER_LINK_PROBE)
#error "the CACS codegen probe must be compiled with RCO_CACS"
#endif

#if !defined(RCO_EXPECT_PRESERVE_NONE)
#error "the CACS codegen probe requires an expected preserve_none mode"
#elif RCO_EXPECT_PRESERVE_NONE && !defined(RCO_CACS_PRESERVE_NONE)
#error "the preserve_none definition did not reach the probe caller"
#elif !RCO_EXPECT_PRESERVE_NONE && defined(RCO_CACS_PRESERVE_NONE)
#error "the plain CACS probe unexpectedly enabled preserve_none"
#endif

#if defined(RCO_NORMAL_CALLER_LINK_PROBE)
int main(void)
{
    unsigned ready_events = 0;
    return rco_yield() +
           rco_wait_fd(-1, RCO_EVENT_READ, 0, &ready_events) +
           rco_sleep_ms(0);
}
#else
#if !defined(RCO_RAW_SWITCH_PROBE_ONLY)
__attribute__((noinline)) int rco_cacs_codegen_probe(void)
{
    return rco_yield();
}
#endif

__attribute__((noinline)) void
rco_cacs_raw_switch_probe(struct rco_context *from,
                          const struct rco_context *to)
{
    rco_context_switch(from, to);
}
#endif
