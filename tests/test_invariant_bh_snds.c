#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>

/* Include the production source directly */
#include "source/src/avp/bh_snds.c"

static jmp_buf jump_buffer;

static void segfault_handler(int sig) {
    longjmp(jump_buffer, 1);
}

START_TEST(test_buffer_reads_within_declared_length)
{
    /* Invariant: buffer reads never exceed declared length;
       oversized inputs must be truncated or rejected, not overflow */
    const char *payloads[] = {
        /* exact exploit: 2x typical sound name buffer (256 bytes) */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        /* boundary: exactly 255 chars (one under typical 256-byte buffer) */
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
        /* valid short input */
        "valid_sound"
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    struct sigaction sa, old_sa;
    sa.sa_handler = segfault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, &old_sa);

    for (int i = 0; i < num_payloads; i++) {
        if (setjmp(jump_buffer) == 0) {
            /* Call the real production function; if it segfaults we catch it */
            GetSoundID(payloads[i]);
            /* If we reach here, no crash occurred — pass */
            ck_assert_msg(1, "No crash for payload %d", i);
        } else {
            /* A segfault means a buffer overflow occurred — fail the test */
            ck_abort_msg("SIGSEGV detected on payload %d: buffer overflow in GetSoundID", i);
        }
    }

    sigaction(SIGSEGV, &old_sa, NULL);
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_reads_within_declared_length);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}