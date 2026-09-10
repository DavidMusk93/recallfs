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

static int test_accept_error_classification(void)
{
    const int connection_errors[] = {
        ECONNABORTED,
        ENETDOWN,
        EPROTO,
        ENOPROTOOPT,
        EHOSTDOWN,
        ENONET,
        EHOSTUNREACH,
        EOPNOTSUPP,
        ENETUNREACH,
    };
    for (size_t index = 0;
         index < sizeof(connection_errors) / sizeof(connection_errors[0]);
         ++index) {
        CHECK(accept_error_is_connection_local(connection_errors[index]));
        CHECK(!accept_error_needs_retry(connection_errors[index]));
    }

    CHECK(accept_error_needs_retry(EMFILE));
    CHECK(accept_error_needs_retry(ENFILE));
    CHECK(accept_error_needs_retry(ENOBUFS));
    CHECK(accept_error_needs_retry(ENOMEM));
    CHECK(!accept_error_is_connection_local(EBADF));
    CHECK(!accept_error_needs_retry(EBADF));
    return 0;
}

int main(void)
{
    struct l4_app app = {0};
    CHECK(test_accept_error_classification() == 0);
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
