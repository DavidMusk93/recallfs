#define main l4_forwarder_program_main
#include "../examples/l4_forwarder.c"
#undef main

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int never_run(void *argument)
{
    (void)argument;
    return 0;
}

int main(void)
{
    struct l4_app app = {0};
    CHECK(rco_runtime_create(NULL, &app.runtime) == 0);

    app.draining = true;
    CHECK(handle_listener_wait_error(&app, -EIO) == 0);
    CHECK(app.fatal_error == 0);

    app.draining = false;
    CHECK(handle_listener_wait_error(&app, -ECANCELED) == 0);
    CHECK(app.fatal_error == 0);
    CHECK(handle_listener_wait_error(&app, -EBADF) == 0);
    CHECK(app.fatal_error == 0);

    CHECK(handle_listener_wait_error(&app, -EIO) == -EIO);
    CHECK(app.fatal_error == -EIO);
    CHECK(rco_spawn(app.runtime, 0, never_run, NULL, NULL) == -ECANCELED);
    CHECK(rco_runtime_destroy(app.runtime) == 0);
    return 0;
}
