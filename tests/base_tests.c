#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <criterion/criterion.h>

void assert_success(int code) {
    cr_assert_eq(code, EXIT_SUCCESS,
                 "Program exited with %d instead of EXIT_SUCCESS",
		 code);
}

void assert_failure(int code) {
    cr_assert_eq(code, EXIT_FAILURE,
                 "Program exited with %d instead of EXIT_FAILURE",
		 code);
}

void assert_output_matches(int code) {
    cr_assert_eq(code, EXIT_SUCCESS,
                 "Program output did not match reference output.");
}

Test(basecode_suite, cook_basic_test, .timeout=20)
{
    char *cmd = "ulimit -t 10; python3 tests/test_cook.py -c 2 -f rsrc/eggs_benedict.ckb";

    int return_code = WEXITSTATUS(system(cmd));
    assert_success(return_code);
}

Test(basecode_suite, hello_world_test, .timeout=20) {
    char *cmd = "ulimit -t 10; bin/cook -c 1 -f rsrc/hello_world.ckb > tmp/hello_world.out";
    char *cmp = "cmp tmp/hello_world.out tests/rsrc/hello_world.out";

    int return_code = WEXITSTATUS(system(cmd));
    assert_success(return_code);
    return_code = WEXITSTATUS(system(cmp));
    assert_output_matches(return_code);
}
