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
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#define ARENA_SIZE (256u * 1024u)

static int g_pass, g_fail;
static FILE *g_log;

/*
 * Everything printed is also appended to a log that is flushed and fsynced
 * line by line, because the failure this tool exists to survive is the
 * machine stopping. Terminal output can be lost with the session; a synced
 * file names the last thing that was attempted, which after a reboot is the
 * difference between "it hung somewhere" and one code path.
 */
static void
logline(const char *prefix, const char *fmt, va_list ap)
{
	va_list ap2;

	va_copy(ap2, ap);
	printf("%s", prefix);
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);

	if (g_log != NULL) {
		fprintf(g_log, "%s", prefix);
		vfprintf(g_log, fmt, ap2);
		fprintf(g_log, "\n");
		fflush(g_log);
		fsync(fileno(g_log));
	}
	va_end(ap2);
}

/*
 * A breadcrumb dropped BEFORE something that might not return. If the log
 * ends with one of these, that operation is where it stopped.
 */
static void
trace(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	logline("  ..   ", fmt, ap);
	va_end(ap);
}

static void
ok(bool cond, const char *fmt, ...)
{
	va_list ap;

	if (cond) {
		g_pass++;
	} else {
		g_fail++;
	}
	va_start(ap, fmt);
	logline(cond ? "  ok   " : "  FAIL ", fmt, ap);
	va_end(ap);
}

/*
 * The error text for a call that has already returned. Reading errno as a
 * sibling argument of the call that sets it - ok(f() == 0, "%s",
 * strerror(errno)) - is unspecified: clang evaluates the varargs first, so
 * the reported error is the one from before the call. That is how a passing
 * check came to print "Undefined error: 0".
 */
static const char *
lasterr(int r)
{
	static char buf[128];

	if (r == 0) {
		buf[0] = '\0';
	} else {
		snprintf(buf, sizeof(buf), " (%s)", strerror(errno));
	}
	return buf;
}

static void
stage_banner(const char *name)
{
	printf("%s\n", name);
	fflush(stdout);
	if (g_log != NULL) {
		fprintf(g_log, "== %s\n", name);
		fflush(g_log);
		fsync(fileno(g_log));
	}
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

/*
 * Is there anything to read?
 *
 * A blocking BINDER_WRITE_READ with nothing queued sleeps until something
 * arrives, so a driver that stops answering - or answers in a form this
 * tool does not recognise, which is what a protocol change looks like from
 * here - hangs the run instead of failing it. That is exactly what
 * BR_TRANSACTION_SEC_CTX did the first time it appeared. select() is the
 * readiness call that reaches this driver (poll() cannot; see the poll
 * stage), so every wait goes through it first and gives up on a timeout.
 */
static bool
binder_readable(int fd, int ms)
{
	struct timeval tv;
	fd_set rfds;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return select(fd + 1, &rfds, NULL, NULL, &tv) == 1 && FD_ISSET(fd, &rfds);
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
		case BR_TRANSACTION_SEC_CTX:
			/* The same transaction with a security context appended. Its
			 * payload is larger, so a parser that does not know it walks
			 * off into the payload and reads that as commands. */
			plen = sizeof(struct binder_transaction_data_secctx);
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

	/* A driver that carries security contexts answers a transaction with
	 * BR_TRANSACTION_SEC_CTX, whose first member is the transaction data -
	 * so a caller asking for BR_TRANSACTION wants this too. */
	if (cmd == BR_TRANSACTION_SEC_CTX && s->want == BR_TRANSACTION) {
		s->seen = true;
		memcpy(&s->tr, payload, sizeof(s->tr));
		return;
	}
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

	stage_banner("version");
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

	stage_banner("arena");
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

	stage_banner("context manager");
	if (!binder_connect(&a, "/dev/binder")) {
		ok(false, "connect");
		return;
	}
	ok(bioctl(a.fd, BINDER_SET_CONTEXT_MGR, &zero) == 0,
	    "BINDER_SET_CONTEXT_MGR is accepted");

	/*
	 * A second manager must be refused - and the claim has to come from
	 * another process. A process gets one binder descriptor per context
	 * (the minor is what distinguishes one open from another, and it is
	 * bound to the process so that rdev is stable), so a second open here
	 * would fail on EBUSY before the driver was ever asked about the
	 * manager, and the check would pass while testing nothing. Silently
	 * skipping it is how it went missing in the first place.
	 */
	{
		pid_t kid = fork();
		int status = 0;

		if (kid == 0) {
			struct binder_conn c2 = { -1, NULL };
			int rc = 3;

			close(a.fd);            /* not ours; it came with the fork */
			if (binder_connect(&c2, "/dev/binder")) {
				rc = bioctl(c2.fd, BINDER_SET_CONTEXT_MGR, &zero) < 0 ? 0 : 1;
				binder_disconnect(&c2);
			}
			_exit(rc);
		}
		if (kid < 0) {
			ok(false, "fork a second claimant");
		} else {
			waitpid(kid, &status, 0);
			ok(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			    "a second context manager is refused, from another process");
		}
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
	int r;

	stage_banner("oneway self-transaction");
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
	r = binder_wr(c.fd, wbuf, woff, NULL, 0, NULL);
	ok(r == 0, "BC_ACQUIRE on handle 0%s", lasterr(r));

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

	trace("sending BC_TRANSACTION (oneway) and reading back");
	r = binder_wr(c.fd, wbuf, woff, rbuf, sizeof(rbuf), &consumed);
	ok(r == 0, "BC_TRANSACTION accepted%s", lasterr(r));

	memset(&scan, 0, sizeof(scan));
	scan.want = BR_TRANSACTION;
	for_each_br(rbuf, consumed, br_scan_cb, &scan);

	if (!scan.seen && binder_readable(c.fd, 1000)) {
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

		trace("freeing the delivered buffer");
		woff = 0;
		cmd = BC_FREE_BUFFER;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		memcpy(wbuf + woff, &scan.tr.data.ptr.buffer, 8); woff += 8;
		r = binder_wr(c.fd, wbuf, woff, NULL, 0, NULL);
		ok(r == 0, "BC_FREE_BUFFER accepted%s", lasterr(r));

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

		if (!binder_readable(sa->fd, 100)) {
			continue;
		}
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
	int spin, r;

	stage_banner("request and reply");
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

	trace("sending the synchronous transaction (this blocks until the reply)");
	consumed = 0;
	r = binder_wr(c.fd, wbuf, woff, rbuf, sizeof(rbuf), &consumed);
	ok(r == 0, "the synchronous transaction was sent%s", lasterr(r));

	memset(&scan, 0, sizeof(scan));
	scan.want = BR_REPLY;
	for_each_br(rbuf, consumed, br_scan_cb, &scan);
	for (spin = 0; spin < 200 && !scan.seen; spin++) {
		if (!binder_readable(c.fd, 50)) {
			continue;
		}
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

	trace("joining the serving thread");
	pthread_join(tid, NULL);
	trace("closing the binder fd (proc teardown)");
	binder_disconnect(&c);
}

/*
 * Readiness, and the platform limitation behind it.
 *
 * The driver reports readiness through d_select, which is what select(2)
 * asks. poll(2) on macOS does not ask it: it is implemented over kqueue,
 * and filt_specattach refuses a knote on a third-party character device
 * unless the driver has called cdevsw_setkqueueok() - which is not in the
 * SDK and is exported only through com.apple.kpi.private, a KPI the sibling
 * modules deliberately do not link. So poll() answers POLLNVAL here, having
 * never reached this driver at all.
 *
 * The escape hatch is in that same check: a knote carrying NOTE_LOWAT with
 * a low-water mark of 1 is accepted. kqueue therefore works, which is what
 * matters in practice - Android's looper uses epoll, and mSL/NABI
 * implements epoll over kqueue, so its binder shim sets NOTE_LOWAT and gets
 * exactly the readiness this driver reports.
 *
 * All four behaviours are asserted, including the two that are failures of
 * the platform rather than of the driver, so that a kernel which one day
 * fixes them is noticed rather than silently relied upon.
 */
static void
test_poll(void)
{
	struct binder_conn c = { -1, NULL };
	struct binder_transaction_data tr;
	struct pollfd pfd;
	struct kevent kev;
	struct timespec ts = { 0, 200 * 1000 * 1000 };
	struct timeval tv;
	fd_set rfds;
	uint8_t wbuf[128];
	uint8_t payload[8] = "poll";
	uint32_t cmd;
	size_t woff = 0;
	int32_t zero = 0;
	int kq, r;

	stage_banner("poll");
	if (!binder_connect(&c, "/dev/binder")) {
		ok(false, "connect");
		return;
	}

	FD_ZERO(&rfds);
	FD_SET(c.fd, &rfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	r = select(c.fd + 1, &rfds, NULL, NULL, &tv);
	ok(r == 0, "select() says an idle binder fd is not readable");

	pfd.fd = c.fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	r = poll(&pfd, 1, 0);
	ok(r == 1 && (pfd.revents & POLLNVAL) != 0,
	    "poll() answers POLLNVAL - it never reaches the driver (see above)");

	kq = kqueue();
	EV_SET(&kev, c.fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	r = kevent(kq, &kev, 1, NULL, 0, NULL);
	ok(r < 0 && errno == EINVAL,
	    "a plain EVFILT_READ knote is refused, as filt_specattach requires");

	EV_SET(&kev, c.fd, EVFILT_READ, EV_ADD, NOTE_LOWAT, 1, NULL);
	r = kevent(kq, &kev, 1, NULL, 0, NULL);
	ok(r == 0, "EVFILT_READ with NOTE_LOWAT=1 registers%s", lasterr(r));

	if (bioctl(c.fd, BINDER_SET_CONTEXT_MGR, &zero) < 0) {
		ok(false, "become the context manager (%s)", strerror(errno));
		close(kq);
		binder_disconnect(&c);
		return;
	}

	woff = 0;
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
	trace("queueing a transaction, then asking whether the fd is readable");
	binder_wr(c.fd, wbuf, woff, NULL, 0, NULL);

	FD_ZERO(&rfds);
	FD_SET(c.fd, &rfds);
	tv.tv_sec = 0;
	tv.tv_usec = 500 * 1000;
	r = select(c.fd + 1, &rfds, NULL, NULL, &tv);
	ok(r == 1 && FD_ISSET(c.fd, &rfds),
	    "select() says a pending transaction makes the fd readable");

	memset(&kev, 0, sizeof(kev));
	r = kevent(kq, NULL, 0, &kev, 1, &ts);
	ok(r == 1 && kev.filter == EVFILT_READ,
	    "kqueue reports the same work (data %lld)", (long long)kev.data);

	close(kq);
	binder_disconnect(&c);
}

/*
 * Two processes: one holds an object, the other receives it as a handle
 * and asks to be told when it dies. This is the path everything real uses,
 * and the only one that proves translation across a process boundary.
 *
 * The synchronisation is deliberately careful, because the first version
 * was not and it cost a diagnosis. It used one pipe in both directions with
 * both ends open in both processes, so a child that failed early left the
 * parent blocked in read() forever - no EOF, no output, and the child's
 * actual complaint lost. Now there are two pipes, each with the unused end
 * closed on each side, so a child that dies is an EOF rather than a hang;
 * the parent waits with a timeout regardless; and the child reports why it
 * gave up instead of exiting with a bare number.
 */

/* What the child reports back, in its exit status. */
#define KID_OK             0
#define KID_NO_CONNECT     2
#define KID_NO_ACQUIRE     3
#define KID_NO_DEATH       4

static const char *
kid_reason(int code)
{
	switch (code) {
	case KID_OK:          return "it saw BR_DEAD_BINDER with its own cookie";
	case KID_NO_CONNECT:  return "it could not open and register an arena";
	case KID_NO_ACQUIRE:  return "BC_ACQUIRE / BC_REQUEST_DEATH_NOTIFICATION failed";
	case KID_NO_DEATH:    return "no BR_DEAD_BINDER arrived before it gave up";
	default:              return "it died unexpectedly";
	}
}

/* Wait for one byte, or give up. Returns 1 on data, 0 on EOF, -1 on timeout. */
static int
wait_for_byte(int fd, int ms)
{
	struct pollfd pfd;
	char b;
	int r;

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	r = poll(&pfd, 1, ms);
	if (r == 0) {
		return -1;
	}
	if (r < 0) {
		return -1;
	}
	r = (int)read(fd, &b, 1);
	return r > 0 ? 1 : 0;
}

static void
test_two_process(void)
{
	struct binder_conn srv = { -1, NULL };
	int ready[2], go[2];
	pid_t child;
	int32_t zero = 0;
	int status = 0;
	int ready_state;

	stage_banner("two processes");

	if (pipe(ready) != 0 || pipe(go) != 0) {
		ok(false, "create the synchronisation pipes");
		return;
	}
	if (!binder_connect(&srv, "/dev/binder")) {
		ok(false, "the owner connects");
		return;
	}
	if (bioctl(srv.fd, BINDER_SET_CONTEXT_MGR, &zero) < 0) {
		ok(false, "the owner becomes the context manager (%s)", strerror(errno));
		binder_disconnect(&srv);
		return;
	}

	trace("forking the client");
	child = fork();
	if (child < 0) {
		ok(false, "fork");
		binder_disconnect(&srv);
		return;
	}

	if (child == 0) {
		struct binder_conn cli = { -1, NULL };
		uint8_t wbuf[256], rbuf[512];
		struct binder_handle_cookie hc;
		struct br_scan scan;
		uint64_t consumed = 0;
		uint32_t cmd;
		size_t woff = 0;
		int spin, rc;
		char discard;

		/* The parent's descriptors are not ours to hold: the binder fd
		 * came along with the fork, and the pipe ends we do not use
		 * would stop the other side ever seeing an EOF. */
		close(srv.fd);
		close(ready[0]);
		close(go[1]);

		if (!binder_connect(&cli, "/dev/binder")) {
			_exit(KID_NO_CONNECT);
		}

		woff = 0;
		cmd = BC_ACQUIRE;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		memset(wbuf + woff, 0, 4); woff += 4;          /* handle 0 */
		cmd = BC_ENTER_LOOPER;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		cmd = BC_REQUEST_DEATH_NOTIFICATION;
		memcpy(wbuf + woff, &cmd, 4); woff += 4;
		hc.handle = 0;
		hc.cookie = 0xDEADBEEF;
		memcpy(wbuf + woff, &hc, sizeof(hc)); woff += sizeof(hc);

		if (binder_wr(cli.fd, wbuf, woff, NULL, 0, NULL) < 0) {
			_exit(KID_NO_ACQUIRE);
		}

		/* Ready. The parent answers by closing its end, not by writing. */
		if (write(ready[1], "g", 1) != 1) {
			_exit(KID_NO_ACQUIRE);
		}
		(void)read(go[0], &discard, 1);

		memset(&scan, 0, sizeof(scan));
		scan.want = BR_DEAD_BINDER;
		for (spin = 0; spin < 200 && !scan.seen; spin++) {
			if (!binder_readable(cli.fd, 50)) {
				continue;
			}
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
		rc = (scan.seen && scan.cookie == 0xDEADBEEF) ? KID_OK : KID_NO_DEATH;
		binder_disconnect(&cli);
		_exit(rc);
	}

	close(ready[1]);
	close(go[0]);

	trace("waiting for the client to acquire a handle");
	ready_state = wait_for_byte(ready[0], 10000);
	ok(ready_state == 1, "the client acquired a handle and asked about its death%s",
	    ready_state == 0 ? " (it exited first)" :
	    ready_state < 0 ? " (it never answered)" : "");

	if (ready_state == 1) {
		trace("closing the owner's fd - its object dies here");
		binder_disconnect(&srv);
	} else {
		binder_disconnect(&srv);
	}
	close(go[1]);          /* release the client, however it went */

	trace("waiting for the client to report");
	waitpid(child, &status, 0);
	close(ready[0]);

	if (WIFEXITED(status)) {
		int code = WEXITSTATUS(status);

		ok(code == KID_OK, "the client received BR_DEAD_BINDER (%s)",
		    kid_reason(code));
	} else {
		ok(false, "the client died on a signal (%d)", WTERMSIG(status));
	}
}

struct stage {
	const char *name;
	void (*fn)(void);
	const char *what;
};

static const struct stage g_stages[] = {
	{ "version", test_version,         "the devices exist and name their protocol" },
	{ "arena",   test_arena,           "arena registration and its bounds" },
	{ "manager", test_context_manager, "context manager registration, per context" },
	{ "oneway",  test_oneway_self,     "a oneway transaction to ourselves" },
	{ "sync",    test_request_reply,   "request and reply across two threads" },
	{ "poll",    test_poll,            "readiness agreeing with the read loop" },
	{ "twoproc", test_two_process,     "handle transfer and death, two processes" },
};

#define NSTAGES (int)(sizeof(g_stages) / sizeof(g_stages[0]))

static void
usage(void)
{
	int i;

	printf("usage: binder-probe [--stage NAME]... [--list] [--log PATH]\n\n");
	printf("Runs every stage when none is named. The stages are ordered:\n"
	    "each assumes the ones before it work, so run them in order when\n"
	    "walking up to a failure.\n\n");
	for (i = 0; i < NSTAGES; i++) {
		printf("  %-8s %s\n", g_stages[i].name, g_stages[i].what);
	}
	printf("\nEvery line is also appended to a log - out/binder-probe.log,\n"
	    "or ./binder-probe.log - flushed and synced as it is written, so it\n"
	    "survives the machine stopping. A log ending in a '..' line names\n"
	    "the operation that did not return.\n");
}

static void
log_open(const char *path)
{
	char fallback[] = "binder-probe.log";

	if (path == NULL) {
		path = access("out", F_OK) == 0 ? "out/binder-probe.log" : fallback;
	}
	g_log = fopen(path, "a");
	if (g_log != NULL) {
		time_t now = time(NULL);

		fprintf(g_log, "\n=== binder-probe %s", ctime(&now));
		fflush(g_log);
	} else {
		printf("  (could not open %s; output is not being logged)\n", path);
	}
}

int
main(int argc, char **argv)
{
	const char *chosen[NSTAGES];
	const char *logpath = NULL;
	int nchosen = 0;
	int i, j;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		}
		if (strcmp(argv[i], "--stage") == 0 && i + 1 < argc) {
			if (nchosen < NSTAGES) {
				chosen[nchosen++] = argv[++i];
			}
			continue;
		}
		if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
			logpath = argv[++i];
			continue;
		}
		printf("unknown argument: %s\n\n", argv[i]);
		usage();
		return 2;
	}

	setvbuf(stdout, NULL, _IONBF, 0);
	log_open(logpath);

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

	if (nchosen == 0) {
		for (i = 0; i < NSTAGES; i++) {
			g_stages[i].fn();
		}
	} else {
		for (j = 0; j < nchosen; j++) {
			for (i = 0; i < NSTAGES; i++) {
				if (strcmp(chosen[j], g_stages[i].name) == 0) {
					g_stages[i].fn();
					break;
				}
			}
			if (i == NSTAGES) {
				printf("unknown stage: %s (try --list)\n", chosen[j]);
				return 2;
			}
		}
	}

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	if (g_log != NULL) {
		fprintf(g_log, "-- %d passed, %d failed\n", g_pass, g_fail);
		fflush(g_log);
		fsync(fileno(g_log));
		fclose(g_log);
	}
	return g_fail == 0 ? 0 : 1;
}
