/*
 * Self-test fix. Never active in the game: it only switches on when the
 * MXBFIX_SELFTEST environment variable is set, which tools/testhost does.
 *
 * It recognises the custom exception the test host raises and resumes
 * execution, proving the vectored-handler recovery path and the saves counter
 * work end to end. Use it as the template for real fixes.
 */
#include "../fixes.h"

static int install(fix_t *self)
{
    (void)self;
    char v[8];
    return env_get("MXBFIX_SELFTEST", v, sizeof v);
}

static int on_exception(fix_t *self, EXCEPTION_POINTERS *ep)
{
    (void)self;
    /* Identify the fault exactly, or leave it alone. */
    if (ep->ExceptionRecord->ExceptionCode != MXBFIX_SELFTEST_CODE) return 0;
    /* A software exception raised with RaiseException resumes after the call. */
    return 1;
}

fix_t fix_selftest = {
    .id = "selftest",
    .title = "Test-only fix used by tools/testhost",
    .crash_id = NULL,
    .install = install,
    .on_exception = on_exception,
};
