// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "uapi/edu_uapi.h"

_Static_assert(sizeof(struct edu_factorial_req) == 32,
               "unexpected factorial ABI layout");

_Static_assert(sizeof(struct edu_dma_selftest_req) == 16,
               "unexpected DMA ABI layout");

enum test_operation {
    TEST_FACTORIAL,
    TEST_DMA,
};

static void wait_enter(const char *message)
{
    int ch;

    puts(message);
    fflush(stdout);

    do {
        ch = getchar();
    } while (ch != '\n' && ch != EOF);
}

static const char *operation_name(enum test_operation op)
{
    return op == TEST_DMA ? "dma" : "factorial";
}

/*
 * Return 0 on success or a saved errno on ioctl failure.
 * Log the operation and request so different paths cannot
 * accidentally be confused.
 */
static int run_ioctl(int fd, enum test_operation op,
                     unsigned int value)
{
    int ret;

    if (op == TEST_DMA) {
        struct edu_dma_selftest_req req = { 0 };

        req.length = value;

        ret = ioctl(fd, EDU_IOCTL_DMA_SELFTEST, &req);
        if (ret == -1) {
            int saved = errno;

            printf("fd=%d op=dma length=%u errno=%d (%s)\n",
                   fd, value, saved, strerror(saved));
            fflush(stdout);
            return saved;
        }

        printf("fd=%d op=dma length=%u PASS\n", fd, value);
    } else {
        struct edu_factorial_req req = { 0 };

        req.input = value;

        ret = ioctl(fd, EDU_IOCTL_FACTORIAL, &req);
        if (ret == -1) {
            int saved = errno;

            printf("fd=%d op=factorial input=%u "
                   "errno=%d (%s)\n",
                   fd, value, saved, strerror(saved));
            fflush(stdout);
            return saved;
        }

        printf("fd=%d op=factorial input=%u result=%u\n",
               fd, value, req.result);
    }

    fflush(stdout);
    return 0;
}

static bool expect_error(const char *label,
                         int actual, int expected)
{
    if (actual == expected) {
        printf("PASS: %s\n", label);
        return true;
    }

    fprintf(stderr,
            "FAIL: %s: expected errno=%d (%s), "
            "got=%d (%s)\n",
            label,
            expected, strerror(expected),
            actual, actual ? strerror(actual) : "success");
    return false;
}

static int run_smoke(int fd, enum test_operation op)
{
    static const unsigned int dma_sizes[] = {
        1, 63, 64, 65, 255, 256, 257, 4096
    };
    unsigned int value;
    size_t i;
    unsigned int iteration;

    if (op == TEST_DMA) {
        for (i = 0;
             i < sizeof(dma_sizes) / sizeof(dma_sizes[0]);
             i++) {
            if (run_ioctl(fd, op, dma_sizes[i]))
                return EXIT_FAILURE;
        }
    }

    value = op == TEST_DMA ? 4096 : 12;

    for (iteration = 0; iteration < 100; iteration++) {
        if (run_ioctl(fd, op, value))
            return EXIT_FAILURE;
    }

    printf("PASS: %s smoke\n", operation_name(op));
    return EXIT_SUCCESS;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s smoke|stale|active|timeout "
            "[factorial|dma]\n",
            prog);
}

int main(int argc, char **argv)
{
    enum test_operation op = TEST_FACTORIAL;
    unsigned int value;
    int fd1;
    int fd2;
    int fd3;
    int duplicate;
    int errors = 0;
    int result;

    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "smoke") &&
        strcmp(argv[1], "stale") &&
        strcmp(argv[1], "active") &&
        strcmp(argv[1], "timeout")) {
        usage(argv[0]);
        return 2;
    }

    if (argc == 3) {
        if (!strcmp(argv[2], "dma"))
            op = TEST_DMA;
        else if (strcmp(argv[2], "factorial")) {
            usage(argv[0]);
            return 2;
        }
    }

    value = op == TEST_DMA ? 256 : 12;

    printf("scenario=%s operation=%s\n",
           argv[1], operation_name(op));

    fd1 = open("/dev/edu0", O_RDWR);
    if (fd1 == -1) {
        perror("open /dev/edu0");
        return EXIT_FAILURE;
    }

    if (!strcmp(argv[1], "smoke")) {
        result = run_smoke(fd1, op);
        close(fd1);
        return result;
    }

    if (!strcmp(argv[1], "timeout")) {
        int first;
        int second;
        bool first_ok;
        bool second_ok;

        /*
         * Both requests use the same open fd and binding.
         * Do not rebind or reload between them.
         */
        first = run_ioctl(fd1, op, value);
        second = run_ioctl(fd1, op, value);

        first_ok = expect_error(
            "first request times out",
            first, ETIMEDOUT);

        second_ok = expect_error(
            "second request sees fault latch",
            second, EIO);

        close(fd1);

        return first_ok && second_ok
            ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (!strcmp(argv[1], "active")) {
        int err = run_ioctl(fd1, op, value);

        result = expect_error(
            "active request cancelled by disconnect",
            err, ENODEV)
            ? EXIT_SUCCESS : EXIT_FAILURE;

        wait_enter(
            "ioctl finished; fd remains open. "
            "Press Enter to close.");

        close(fd1);
        return result;
    }

    fd2 = open("/dev/edu0", O_RDWR);
    fd3 = open("/dev/edu0", O_RDWR);
    duplicate = dup(fd1);

    if (fd2 == -1 || fd3 == -1 || duplicate == -1) {
        fprintf(stderr, "failed to acquire test descriptors\n");

        if (duplicate != -1)
            close(duplicate);
        if (fd3 != -1)
            close(fd3);
        if (fd2 != -1)
            close(fd2);

        close(fd1);
        return EXIT_FAILURE;
    }

    wait_enter(
        "Three opens and one dup are ready. "
        "Unbind in another terminal, then press Enter.");

    errors += !expect_error(
        "fd1 after disconnect",
        run_ioctl(fd1, op, value), ENODEV);

    errors += !expect_error(
        "fd2 after disconnect",
        run_ioctl(fd2, op, value), ENODEV);

    errors += !expect_error(
        "fd3 after disconnect",
        run_ioctl(fd3, op, value), ENODEV);

    errors += !expect_error(
        "duplicate after disconnect",
        run_ioctl(duplicate, op, value), ENODEV);

    close(fd1);
    close(fd2);
    close(fd3);

    wait_enter(
        "Only dup(fd1) remains. "
        "Object must still be alive. "
        "Press Enter to close the last description.");

    close(duplicate);

    return errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
