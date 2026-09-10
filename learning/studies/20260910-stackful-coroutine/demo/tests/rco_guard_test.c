#include "rco.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

__attribute__((noinline)) static uint64_t consume_stack(uint64_t depth)
{
    volatile unsigned char frame[4096];
    for (size_t index = 0; index < sizeof(frame); ++index) {
        frame[index] = (unsigned char)(depth + index);
    }
    uint64_t child = consume_stack(depth + 1);
    return child + frame[depth % sizeof(frame)];
}

static int overflow_worker(void *argument)
{
    (void)argument;
    return (int)consume_stack(1);
}

int main(void)
{
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }
    if (child == 0) {
        struct rco_config config = {
            .default_stack_size = RCO_STACK_SIZE_MIN,
            .max_coroutines = 1,
            .max_fds = 16,
            .stack_cache_bytes = 0,
        };
        struct rco_runtime *runtime = NULL;
        if (rco_runtime_create(&config, &runtime) != 0 ||
            rco_spawn(runtime, 0, overflow_worker, NULL, NULL) != 0) {
            _exit(40);
        }
        (void)rco_runtime_run(runtime);
        _exit(41);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        perror("waitpid");
        return EXIT_FAILURE;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        fprintf(stderr, "expected guard-page SIGSEGV, status=%d\n", status);
        return EXIT_FAILURE;
    }

    puts("rco guard-page test passed");
    return EXIT_SUCCESS;
}
