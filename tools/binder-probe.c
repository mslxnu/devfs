/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder-probe.c
 *
 * Userspace smoke test: open /dev/binder, call BINDER_VERSION ioctl,
 * verify it returns protocol version 8. This is the M0 acceptance test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>

#include <fs/devfs/binder.h>

int
main(int argc, char *argv[])
{
    int fd;
    struct binder_version ver;
    int ret;

    (void)argc;
    (void)argv;

    fd = open("/dev/binder", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open(/dev/binder) failed: %s\n", strerror(errno));
        return 1;
    }

    ret = ioctl(fd, BINDER_VERSION, &ver);
    if (ret < 0) {
        fprintf(stderr, "FAIL: ioctl(BINDER_VERSION) failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("BINDER_VERSION: protocol_version=%d\n", ver.protocol_version);

    if (ver.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION) {
        fprintf(stderr, "FAIL: expected protocol version %d, got %d\n",
                BINDER_CURRENT_PROTOCOL_VERSION, ver.protocol_version);
        close(fd);
        return 1;
    }

    printf("PASS: BINDER_VERSION returns %d\n", BINDER_CURRENT_PROTOCOL_VERSION);

    close(fd);
    return 0;
}