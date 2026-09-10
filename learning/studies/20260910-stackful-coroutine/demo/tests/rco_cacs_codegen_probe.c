#include "rco.h"

#if !defined(RCO_CACS)
#error "the CACS codegen probe must be compiled with RCO_CACS"
#endif

#if !defined(RCO_EXPECT_PRESERVE_NONE)
#error "the CACS codegen probe requires an expected preserve_none mode"
#elif RCO_EXPECT_PRESERVE_NONE && !defined(RCO_CACS_PRESERVE_NONE)
#error "the preserve_none definition did not reach the probe caller"
#elif !RCO_EXPECT_PRESERVE_NONE && defined(RCO_CACS_PRESERVE_NONE)
#error "the plain CACS probe unexpectedly enabled preserve_none"
#endif

__attribute__((noinline)) int rco_cacs_codegen_probe(void)
{
    return rco_yield();
}
