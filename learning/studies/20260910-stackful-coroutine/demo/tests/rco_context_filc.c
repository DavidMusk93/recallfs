#include "rco_internal.h"

#include <stdlib.h>

void rco_context_capture_fp(struct rco_context *context)
{
    context->mxcsr = 0x1f80;
    context->x87_control = 0x037f;
}

void rco_context_switch(struct rco_context *from,
                        const struct rco_context *to)
{
    (void)from;
    (void)to;
    abort();
}
