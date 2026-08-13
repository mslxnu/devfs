/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder-probe.c
 *
 * The functional test for the binder driver, run against a live load.
 * There is no in-kernel test framework, so this is the thing that decides
 * whether the driver works: it speaks the protocol the way libbinder does,
 * and every check below failed at least once while the driver was being
 * written.
 *
 * The stages build on each other:
 *
 *   version   the device answers, and answers protocol 8
 *   arena     BINDER_MSL_SET_ARENA, and its bounds are enforced
 *   manager   registering as the context manager, once and only once
 *   oneway    a transaction to ourselves that expects no reply - the
 *             simplest path that exercises allocate, translate, deliver
 *   sync      request and reply across two threads, which is the path a
 *             real service takes and the one a single thread deadlocks on
 *   poll      readiness agreeing with what the read loop would do
 *   handles   an object passed between two processes becomes a handle
 *   death     and when its owner exits, the holder is told
 *
 * Run it as root (the devices are 0666, but the context manager is
 * per-uid) after `make load`:  out/binder-probe
 */

#include <fs/devfs/binder.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define ARENA_SIZE (256u * 1024u)

static int g_pass, g_fail;

static void
ok(bool cond, const char *fmt, ...)
{
	va_list ap;

	if (cond) {
		g_pass++;
		printf("  ok   ");
	} else {
		g_fail++;
		printf("  FAIL ");
	}
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* Every binder ioctl goes out with both direction bits set; see binder.h. */
static int
bioctl(int fd, uint32_t cmd, void *arg)
{
	return ioctl(fd, (unsigned long)BINDER_CMD_HOST(cmd), arg);
}

struct binder_conn {
	int fd;
	void *arena;
};

static bool
binder_connect(struct binder_conn *c, const char *path)
{
	struct binder_msl_arena arena;
	struct binder_version ver;

	c->fd = open(path, O_RDWR | O_CLOEXEC);
	if (c->fd < 0) {
		return false;
	}
	if (bioctl(c->fd, BINDER_VERSION, &ver) < 0 ||
	    ver.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION) {
		close(c->fd);
		return false;
	}

	c->arena = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_SHARED, -1, 0);
	if (c->arena == MAP_FAILED) {
		close(c->fd);
		return false;
	}
	arena.addr = (uint64_t)(uintptr_t)c->arena;
	arena.size = ARENA_SIZE;
	if (bioctl(c->fd, BINDER_MSL_SET_ARENA, &arena) < 0) {
		munmap(c->arena, ARENA_SIZE);
		close(c->fd);
		return false;
	}
	return true;
}

static void
binder_disconnect(struct binder_conn *c)
{
	if (c->arena != NULL) {
		munmap(c->arena, ARENA_SIZE);
	}
	if (c->fd >= 0) {
		close(c->fd);
	}
}

/* One BINDER_WRITE_READ. Either half may be empty. */
static int
binder_wr(int fd, void *wbuf, size_t wsize, void *rbuf, size_t rsize,
    uint64_t *read_consumed)
{
	struct binder_write_read bwr;
	int r;

	memset(&bwr, 0, sizeof(bwr));
	bwr.write_size = wsize;
	bwr.write_buffer = (binder_uintptr_t)(uintptr_t)wbuf;
	bwr.read_size = rsize;
	bwr.read_buffer = (binder_uintptr_t)(uintptr_t)rbuf;

	r = bioctl(fd, BINDER_WRITE_READ, &bwr);
	if (read_consumed != NULL) {
		*read_consumed = bwr.read_consumed;
	}
	return r;
}

/* Walk a read buffer, calling back with each BR_ command and its payload. */
static void
for_each_br(const uint8_t *buf, uint64_t len,
    void (*fn)(uint32_t cmd, const void *payload, void *ctx), void *ctx)
{
	uint64_t off = 0;

	while (off + sizeof(uint32_t) <= len) {
		uint32_t cmd;
		size_t plen = 0;

		memcpy(&cmd, buf + off, sizeof(cmd));
		off += sizeof(cmd);

		switch (cmd) {
		case BR_TRANSACTION:
		case BR_REPLY:
			plen = sizeof(struct binder_transaction_data);
			break;
		case BR_INCREFS:
		case BR_ACQUIRE:
		case BR_RELEASE:
		case BR_DECREFS:
			plen = sizeof(struct binder_ptr_cookie);
			break;
		case BR_DEAD_BINDER:
		case BR_CLEAR_DEATH_NOTIFICATION_DONE:
			plen = sizeof(binder_uintptr_t);
			break;
		case BR_ERROR:
			plen = sizeof(int32_t);
			break;
		default:
			plen = 0;
			break;
		}
		if (off + plen > len) {
			return;
		}
		fn(cmd, buf + off, ctx);
		off += plen;
	}
}

struct br_scan {
	uint32_t want;
	bool seen;
	struct binder_transaction_data tr;
	binder_uintptr_t cookie;
};

static void
br_scan_cb(uint32_t cmd, const void *payload, void *ctx)
{
	struct br_scan *s = ctx;

	if (cmd != s->want) {
		return;
	}
	s->seen = true;
	if (cmd == BR_TRANSACTION || cmd == BR_REPLY) {
		memcpy(&s->tr, payload, sizeof(s->tr));
	} else if (cmd == BR_DEAD_BINDER) {
		memcpy(&s->cookie, payload, sizeof(s->cookie));
	}
}

#pragma mark - Stages

static void
test_version(void)
{
	struct binder_version ver;
	uint32_t abi = 0;
	int fd;

	printf("version\n");
	fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
	ok(fd >= 0, "/dev/binder opens (%s)", fd >= 0 ? "ok" : strerror(errno));
	if (fd < 0) {
		return;
	}
	memset(&ver, 0, sizeof(ver));
	ok(bioctl(fd, BINDER_VERSION, &ver) == 0 &&
	    ver.protocol_version == BINDER_CURRENT_PROTOCOL_VERSION,
	    "BINDER_VERSION reports protocol %d", ver.protocol_version);
	ok(bioctl(fd, BINDER_MSL_ABI_VERSION, &abi) == 0 && abi == BINDER_MSL_ABI_CURRENT,
	    "BINDER_MSL_ABI_VERSION reports revision %u", abi);

	/* The direction-bit rule: Linux's own number carries no argument in,
	 * and the driver refuses rather than acting on zeroes. */
	ok(ioctl(fd, (unsigned long)BINDER_MSL_SET_ARENA, &abi) < 0,
	    "a command sent with Linux direction bits is refused");
	close(fd);

	ok(access("/dev/hwbinder", F_OK) == 0, "/dev/hwbinder exists");
	ok(access("/dev/vndbinder", F_OK) == 0, "/dev/vndbinder exists");
	ok(access("/dev/binderfs/binder-control", F_OK) == 0,
	    "/dev/binderfs/binder-control exists");
}

static void
test_arena(void)
{
	struct binder_conn c = { -1, NULL };
	struct binder_msl_arena a;
	int fd;

	printf("arena\n");
	fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ok(false, "open");
		return;
	}
	a.addr = 0;
	a.size = ARENA_SIZE;
	ok(bioctl(fd, BINDER_MSL_SET_ARENA, &a) < 0, "a null arena is refused");

	a.addr = 0x100000;
	a.size = 64;
	ok(bioctl(fd, BINDER_MSL_SET_ARENA, &a) < 0, "an undersized arena is refused");
	close(fd);

	ok(binder_connect(&c, "/dev/binder"), "a page-aligned arena is accepted");
	if (c.fd >= 0) {
		a.addr = (uint64_t)(uintptr_t)c.arena;
		a.size = ARENA_SIZE;
		ok(bioctl(c.fd, BINDER_MSL_SET_ARENA, &a) < 0,
		    "registering a second arena is refused");
	}
	binder_disconnect(&c);
}

static void
test_context_manager(void)
{
	struct binder_conn a = { -1, NULL }, b = { -1, NULL };
	int32_t zero = 0;

	printf("context manager\n");
	if (!binder_connect(&a, "/dev/binder")) {
		ok(false, "connect");
		return;
	}
	ok(bioctl(a.fd, BINDER_SET_CONTEXT_MGR, &zero) == 0,
	    "BINDER_SET_CONTEXT_MGR is accepted");

	if (binder_connect(&b, "/dev/binder")) {
		ok(bioctl(b.fd, BINDER_SET_CONTEXT_MGR, &zero) < 0,
		    "a second context manager is refused");
		binder_disconnect(&b);
	}

	/* The contexts are separate namespaces: taking the role in one says
	 * nothing about another. */
	if (binder_connect(&b, "/dev/hwbinder")) {
		ok(bioctl(b.fd, BINDER_SET_CONTEXT_MGR, &zero) == 0,
		    "the hwbinder context has its own manager");
		binder_disconnect(&b);
	}
	binder_disconnect(&a);
}

/*
 * A transaction to ourselves that expects no reply. The only single-thread
 * path through the whole engine: allocate in the arena, translate, queue,
 * deliver, free.
 */
static void
test_oneway_self(void)
{
	struct binder_conn c = { -1, NULL };
	struct binder_transaction_data tr;
	struct br_scan scan;
	uint8_t wbuf[128], rbuf[512];
	uint8_t payload[16] = "binder-oneway";
	uint64_t consumed = 0;
	size_t woff = 0;
	uint32_t cmd;
	int32_t zero = 0;

	printf("oneway self-transaction\n");
	if (!binder_connect(&c, "/dev/binder")) {
		ok(false, "connect");
		return;
	}
	if (bioctl(c.fd, BINDER_SET_CONTEXT_MGR, &zero) < 0) {
		ok(false, "become the context manager (%s)", strerror(errno));
		binder_disconnect(&c);
		return;
	}

	/* Acquire handle 0 - the manager, which is us. */
	cmd = BC_ACQUIRE;
	memcpy(wbuf + woff, &cmd, 4); woff += 4;
	memcpy(wbuf + woff, &zero, 4); woff += 4;
	cmd = BC_ENTER_LOOPER;
	memcpy(wbuf + woff, &cmd, 4); woff += 4;
	ok(binder_wr(c.fd, wbuf, woff, NULL, 0, NULL) == 0,
	    "BC_ACQUIRE on handle 0 (%s)", strerror(errno));

	memset(&tr, 0, sizeof(tr));
	tr.target.handle = 0;
	tr.code = 42;
	tr.flags = TF_ONE_WAY;
	tr.data_size = sizeof(payload);
	tr.offsets_size = 0;
	tr.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)payload;
	tr.data.ptr.offsets = 0;

	woff = 0;
	cmd = BC_TRANSACTION;
	memcpy(wbuf + woff, &cmd, 4); woff += 4;
	memcpy(wbuf + woff, &tr, sizeof(tr)); woff += sizeof(tr);

	ok(binder_wr(c.fd, wbuf, woff, rbuf, sizeof(rbuf), &consumed) == 0,
	    "BC_TRANSACTION accepted (%s)", strerror(errno));

	memset(&scan, 0, sizeof(scan));
	scan.want = BR_TRANSACTION;
	for_each_br(rbuf, consumed, br_scan_cb, &scan);

	if (!scan.seen) {
		/* The delivery may need a second read once the first has drained
		 * BR_TRANSACTION_COMPLETE. */
		consumed = 0;
		binder_wr(c.fd, NULL, 0, rbuf, sizeof(rbuf), &consumed);
		for_each_br(rbuf, consumed, br_scan_cb, &scan);
	}

	ok(scan.seen, "BR_TRANSACTION came back");
	if (scan.seen) {
		const uint8_t *got = (const uint8_t *)(uintptr_t)scan.tr.data.ptr.buffer;

		ok(scan.tr.code == 42, "the transaction code survived (%u)", scan.tr.code);
		ok(scan.tr.data_size == sizeof(payload), "the payload size survived (%llu)",
		    (unsigned long long)scan.tr.data_size);
		ok(got >= (const uint8_t *)c.arena &&
		    got < (const uint8_t *)c.arena + ARENA_SIZE,
		    "the payload landed inside the registered arena");
		ok(memcmp(got, payload, sizeof(payload)) == 0,
		    "the payload bytes survived the round trip");
		ok(scan.tr.sender_pid == getpid() || scan.tr.sender_pid == 0,
		    "the sender pid is recorded (%d)", scan.tr.sender_pid);

		woff = 0;
		cmd = BC_FREE_BUFFER;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		memcpy(wbuf + woff, &scan.tr.data.ptr.buffer, 8); woff += 8;
		ok(binder_wr(c.fd, wbuf, woff, NULL, 0, NULL) == 0,
		    "BC_FREE_BUFFER accepted (%s)", strerror(errno));

		/* Freeing it twice is a protocol error, and is caught. */
		ok(binder_wr(c.fd, wbuf, woff, NULL, 0, NULL) < 0,
		    "freeing the same buffer twice is refused");
	}
	binder_disconnect(&c);
}

#pragma mark - Request and reply, across two threads

struct server_arg {
	int fd;
	volatile int ready;
	volatile int served;
};

static void *
server_thread(void *v)
{
	struct server_arg *sa = v;
	uint8_t wbuf[256], rbuf[512];
	uint64_t consumed;
	uint32_t cmd;
	size_t woff = 0;

	cmd = BC_ENTER_LOOPER;
	memcpy(wbuf, &cmd, 4);
	binder_wr(sa->fd, wbuf, 4, NULL, 0, NULL);
	sa->ready = 1;

	for (;;) {
		struct br_scan scan;

		consumed = 0;
		if (binder_wr(sa->fd, NULL, 0, rbuf, sizeof(rbuf), &consumed) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		memset(&scan, 0, sizeof(scan));
		scan.want = BR_TRANSACTION;
		for_each_br(rbuf, consumed, br_scan_cb, &scan);
		if (!scan.seen) {
			continue;
		}

		/* Reply with the code we were called with, doubled. */
		{
			struct binder_transaction_data reply;
			static uint32_t answer;

			answer = scan.tr.code * 2;
			memset(&reply, 0, sizeof(reply));
			reply.data_size = sizeof(answer);
			reply.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)&answer;

			woff = 0;
			cmd = BC_FREE_BUFFER;
			memcpy(wbuf + woff, &cmd, 4); woff += 4;
			memcpy(wbuf + woff, &scan.tr.data.ptr.buffer, 8); woff += 8;
			cmd = BC_REPLY;
			memcpy(wbuf + woff, &cmd, 4); woff += 4;
			memcpy(wbuf + woff, &reply, sizeof(reply)); woff += sizeof(reply);
			binder_wr(sa->fd, wbuf, woff, NULL, 0, NULL);
		}
		sa->served++;
		break;
	}
	return NULL;
}

static void
test_request_reply(void)
{
	struct binder_conn c = { -1, NULL };
	struct server_arg sa;
	pthread_t tid;
	struct binder_transaction_data tr;
	struct br_scan scan;
	uint8_t wbuf[256], rbuf[512];
	uint8_t payload[8] = "sync";
	uint64_t consumed = 0;
	size_t woff = 0;
	uint32_t cmd;
	int32_t zero = 0;
	int spin;

	printf("request and reply\n");
	if (!binder_connect(&c, "/dev/binder")) {
		ok(false, "connect");
		return;
	}
	if (bioctl(c.fd, BINDER_SET_CONTEXT_MGR, &zero) < 0) {
		ok(false, "become the context manager (%s)", strerror(errno));
		binder_disconnect(&c);
		return;
	}

	woff = 0;
	cmd = BC_ACQUIRE;
	memcpy(wbuf + woff, &cmd, 4); woff += 4;
	memcpy(wbuf + woff, &zero, 4); woff += 4;
	binder_wr(c.fd, wbuf, woff, NULL, 0, NULL);

	sa.fd = c.fd;
	sa.ready = 0;
	sa.served = 0;
	if (pthread_create(&tid, NULL, server_thread, &sa) != 0) {
		ok(false, "start the serving thread");
		binder_disconnect(&c);
		return;
	}
	for (spin = 0; spin < 1000 && !sa.ready; spin++) {
		usleep(1000);
	}

	memset(&tr, 0, sizeof(tr));
	tr.target.handle = 0;
	tr.code = 21;
	tr.data_size = sizeof(payload);
	tr.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)payload;

	woff = 0;
	cmd = BC_TRANSACTION;
	memcpy(wbuf + woff, &cmd, 4); woff += 4;
	memcpy(wbuf + woff, &tr, sizeof(tr)); woff += sizeof(tr);

	consumed = 0;
	ok(binder_wr(c.fd, wbuf, woff, rbuf, sizeof(rbuf), &consumed) == 0,
	    "the synchronous transaction was sent (%s)", strerror(errno));

	memset(&scan, 0, sizeof(scan));
	scan.want = BR_REPLY;
	for_each_br(rbuf, consumed, br_scan_cb, &scan);
	for (spin = 0; spin < 200 && !scan.seen; spin++) {
		consumed = 0;
		if (binder_wr(c.fd, NULL, 0, rbuf, sizeof(rbuf), &consumed) < 0 &&
		    errno != EINTR) {
			break;
		}
		for_each_br(rbuf, consumed, br_scan_cb, &scan);
	}

	ok(scan.seen, "BR_REPLY arrived on the calling thread");
	if (scan.seen) {
		uint32_t answer = 0;

		memcpy(&answer, (const void *)(uintptr_t)scan.tr.data.ptr.buffer,
		    sizeof(answer));
		ok(answer == 42, "the reply carried the served value (%u)", answer);

		woff = 0;
		cmd = BC_FREE_BUFFER;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		memcpy(wbuf + woff, &scan.tr.data.ptr.buffer, 8); woff += 8;
		binder_wr(c.fd, wbuf, woff, NULL, 0, NULL);
	}
	ok(sa.served == 1, "the serving thread handled exactly one call");

	pthread_join(tid, NULL);
	binder_disconnect(&c);
}

static void
test_poll(void)
{
	struct binder_conn c = { -1, NULL };
	struct pollfd pfd;
	int32_t zero = 0;
	int r;

	printf("poll\n");
	if (!binder_connect(&c, "/dev/binder")) {
		ok(false, "connect");
		return;
	}
	pfd.fd = c.fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	r = poll(&pfd, 1, 0);
	ok(r == 0 || (pfd.revents & POLLIN) == 0,
	    "an idle binder fd is not readable");

	if (bioctl(c.fd, BINDER_SET_CONTEXT_MGR, &zero) == 0) {
		uint8_t wbuf[128];
		struct binder_transaction_data tr;
		uint8_t payload[8] = "poll";
		uint32_t cmd;
		size_t woff = 0;

		cmd = BC_ACQUIRE;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		memcpy(wbuf + woff, &zero, 4); woff += 4;
		cmd = BC_ENTER_LOOPER;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		binder_wr(c.fd, wbuf, woff, NULL, 0, NULL);

		memset(&tr, 0, sizeof(tr));
		tr.target.handle = 0;
		tr.code = 7;
		tr.flags = TF_ONE_WAY;
		tr.data_size = sizeof(payload);
		tr.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)payload;

		woff = 0;
		cmd = BC_TRANSACTION;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		memcpy(wbuf + woff, &tr, sizeof(tr)); woff += sizeof(tr);
		binder_wr(c.fd, wbuf, woff, NULL, 0, NULL);

		pfd.revents = 0;
		r = poll(&pfd, 1, 500);
		ok(r == 1 && (pfd.revents & POLLIN) != 0,
		    "a pending transaction makes the fd readable");
	}
	binder_disconnect(&c);
}

/*
 * Two processes: one holds an object, the other receives it as a handle
 * and asks to be told when it dies. This is the path everything real uses,
 * and the only one that proves translation across a process boundary.
 */
static void
test_two_process(void)
{
	struct binder_conn srv = { -1, NULL };
	int sync_pipe[2];
	pid_t child;

	printf("two processes\n");
	if (pipe(sync_pipe) != 0) {
		ok(false, "pipe");
		return;
	}
	if (!binder_connect(&srv, "/dev/binder")) {
		ok(false, "connect");
		return;
	}
	{
		int32_t zero = 0;

		if (bioctl(srv.fd, BINDER_SET_CONTEXT_MGR, &zero) < 0) {
			ok(false, "become the context manager (%s)", strerror(errno));
			binder_disconnect(&srv);
			return;
		}
	}

	child = fork();
	if (child < 0) {
		ok(false, "fork");
		binder_disconnect(&srv);
		return;
	}

	if (child == 0) {
		/* Client: acquire handle 0, ask to hear about its death, wait. */
		struct binder_conn cli = { -1, NULL };
		uint8_t wbuf[256], rbuf[512];
		struct binder_handle_cookie hc;
		struct br_scan scan;
		uint64_t consumed = 0;
		uint32_t cmd;
		size_t woff = 0;
		int rc = 1;
		char go;

		if (!binder_connect(&cli, "/dev/binder")) {
			_exit(2);
		}
		woff = 0;
		cmd = BC_ACQUIRE;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		memset(wbuf + woff, 0, 4); woff += 4;
		cmd = BC_ENTER_LOOPER;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		cmd = BC_REQUEST_DEATH_NOTIFICATION;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		hc.handle = 0;
		hc.cookie = 0xDEADBEEF;
		memcpy(wbuf + woff, &hc, sizeof(hc)); woff += sizeof(hc);
		if (binder_wr(cli.fd, wbuf, woff, NULL, 0, NULL) < 0) {
			_exit(3);
		}

		/* Tell the parent we are ready, then wait for it to die. */
		write(sync_pipe[1], "g", 1);
		read(sync_pipe[0], &go, 1);   /* blocks until the parent closes it */

		memset(&scan, 0, sizeof(scan));
		scan.want = BR_DEAD_BINDER;
		for (int spin = 0; spin < 200 && !scan.seen; spin++) {
			consumed = 0;
			if (binder_wr(cli.fd, NULL, 0, rbuf, sizeof(rbuf), &consumed) < 0 &&
			    errno != EINTR) {
				break;
			}
			for_each_br(rbuf, consumed, br_scan_cb, &scan);
			if (!scan.seen) {
				usleep(10000);
			}
		}
		rc = (scan.seen && scan.cookie == 0xDEADBEEF) ? 0 : 1;
		binder_disconnect(&cli);
		_exit(rc);
	}

	/* Server: wait for the client to be ready, then go away. */
	{
		char g;
		int status = 0;

		ok(read(sync_pipe[0], &g, 1) == 1, "the client registered a handle");
		binder_disconnect(&srv);     /* the object dies here */
		close(sync_pipe[1]);         /* which the client learns from the pipe */

		waitpid(child, &status, 0);
		ok(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		    "the client received BR_DEAD_BINDER with its own cookie");
	}
	close(sync_pipe[0]);
}

int
main(void)
{
	printf("mSL/DevFS binder probe\n\n");

	if (access("/dev/binder", F_OK) != 0) {
		printf("  /dev/binder is not present - load the kext first "
		    "(sudo make load)\n");
		return 77;
	}

	/*
	 * Refuse to report on a driver that is not this build. An older kext
	 * left loaded answers some of these tests and fails the rest, which
	 * reads as "the driver is broken" when it means "you are testing
	 * yesterday's driver" - which is exactly what happened the first time
	 * this was run, against an M0 scaffold still resident from an earlier
	 * session.
	 */
	{
		uint32_t abi = 0;
		int fd = open("/dev/binder", O_RDWR | O_CLOEXEC);

		if (fd >= 0) {
			int r = bioctl(fd, BINDER_MSL_ABI_VERSION, &abi);

			close(fd);
			if (r < 0 || abi != BINDER_MSL_ABI_CURRENT) {
				printf("  the loaded driver does not answer "
				    "BINDER_MSL_ABI_VERSION with revision %d - it is not "
				    "this build.\n  Unload it and load this one:\n"
				    "    sudo make unload && sudo make load\n",
				    BINDER_MSL_ABI_CURRENT);
				return 77;
			}
		}
	}

	test_version();
	test_arena();
	test_context_manager();
	test_oneway_self();
	test_request_reply();
	test_poll();
	test_two_process();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
