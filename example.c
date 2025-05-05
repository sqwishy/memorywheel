#define _GNU_SOURCE // memfd_create

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if WITH_LIBUV
#include <uv.h>
#endif

#include "scm.h"
#include "memorywheel.h"

/* the sock tests are also limited by the socket buffer
 * because of SOCK_SEQPACKET (sysctl net.core.wmem_max) */
#define WHEEL_SIZE     ((uint64_t) 128 * 1024)
#define SEND_SIZE_MAX  ((uint64_t)         16)
#define MAGIC          ("¯\\_(ツ)_/¯")
#define NLOOPS         (1000 * 1000 * 1)

#define NANOS_PER_SEC 1000000000

typedef struct timespec timespec_t;

typedef struct err {
	uint16_t  line;
	char     *msg;
	int       eno;
} err_t;

#define err(s)        (err_t) { __LINE__, (s), errno }
#define thiserr(e, s) (err_t) { __LINE__, (s), e }
#define YIPPIE        (err_t) { 0 }
#define iserr(e)      (e).msg != 0
#define eprintln(fmt, ...)  fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define println(fmt, ...)   fprintf(stdout, fmt "\n", ##__VA_ARGS__)
#define ERRFMT              "%s:%i (%i) %s"

#if WITH_LIBUV
#define errfmtargs(e)       (e).msg, (e).line, (e).eno, ((e).eno < 0 ? uv_strerror((e).eno) : strerror((e).eno))
#else
#define errfmtargs(e)       (e).msg, (e).line, (e).eno, strerror((e).eno)
#endif

#define min(x, y)    (((x) > (y)) ? (y) : (x))
#define nelements(v) sizeof((v)) / sizeof((v)[0])

typedef enum tport_e {
	TPORT_SPIN,
	TPORT_LIBUV,
	TPORT_SEQPACKET,
	__TPORT_COUNT,
} tport_t;

/* https://en.wikipedia.org/wiki/Xorshift#xorshiftr+ */

typedef struct xorshiftr128plus_state {
	uint64_t s[2]; // seeds
} xorshiftr128plus_t;

uint64_t
xorshiftr128plus(xorshiftr128plus_t *state)
{
	uint64_t x = state->s[0];
	uint64_t const y = state->s[1];
	state->s[0] = y;
	x ^= x << 23; // shift & xor
	x ^= x >> 17; // shift & xor
	x ^= y; // xor
	state->s[1] = x + y;
	return x;
}

const xorshiftr128plus_t rng_init = { 420, 69 };

err_t
open_memfd(int *memfd)
{
	if ((*memfd = memfd_create("test-memorywheel", MFD_CLOEXEC)) < 0)
		return err("memfd_create");

	if (ftruncate(*memfd, WHEEL_SIZE) < 0) {
		err_t e = err("ftruncate");
		close(*memfd);
		return e;
	}

	return YIPPIE;
}

err_t
open_shm(int memfd, char **shm)
{
	if ((*shm = mmap(NULL, WHEEL_SIZE,
	                 PROT_READ | PROT_WRITE,
	                 MAP_SHARED, memfd, 0)) == MAP_FAILED)
		return err("mmap");
	else
		return YIPPIE;
}

err_t
close_shm(char *shm)
{
	if (munmap(shm, WHEEL_SIZE) < 0)
		return err("munmap");
	else
		return YIPPIE;
}

void
write_buf(char *buf, size_t bufsize)
{
	memset(buf, 0xf0, bufsize);
	memcpy(buf, MAGIC, min(sizeof(MAGIC), bufsize));
}

int
test_buf(const char *buf, size_t bufsize)
{
	return memcmp(buf, MAGIC, min(sizeof(MAGIC), bufsize)) == 0;
}

#ifdef WITH_LIBUV

void
on_uv_disconnected(uv_poll_t *handle, int _status, int _events)
{
	uv_stop(handle->loop);
}

void
on_uv_sigint(uv_signal_t *handle, int _signum)
{
	uv_stop(handle->loop);
}

typedef struct {
	whl_atomic_t *whl;
	whl_efd_t    *whl_efd;
	size_t       *total;
	int           sockfd;
	xorshiftr128plus_t rng;
	uint32_t      loops;
} sender_uv_t;

void
do_uv_send(uv_poll_t *handle, int status, int events)
{
	sender_uv_t       *s = uv_handle_get_data((uv_handle_t*)handle);
	xorshiftr128plus_t rng = s->rng;
	size_t             bufsize = xorshiftr128plus(&rng) % SEND_SIZE_MAX;
	char              *buf;
	whl_offset_t       offset = whl_efd_make_slice(s->whl_efd, &buf, bufsize);

	if (offset == WHL_INVALID_OFFSET)
		return;

	/* only advance rng state if we do the thing */
	s->rng = rng;

	write_buf(buf, bufsize);

	whl_efd_share_slice(s->whl_efd, offset);

	*s->total += bufsize;

	if (!(s->loops--)) {
		uv_stop(handle->loop);
		return;
	}
}

err_t
run_uv_sender(sender_uv_t *s)
{
	int         uverr;
	uv_loop_t  *loop;
	uv_signal_t signal;
	uv_poll_t   poll_sock;
	uv_poll_t   poll_send;

	if (!(loop = uv_default_loop()))
		return thiserr(ENOMEM, "uv_default_loop");

	if (   (uverr = uv_poll_init(loop, &poll_send, s->whl_efd->writable))
	    || (uverr = uv_poll_start(&poll_send, UV_WRITABLE, do_uv_send))
	    || (uverr = uv_poll_init(loop, &poll_sock, s->sockfd))
	    || (uverr = uv_poll_start(&poll_sock, UV_DISCONNECT, on_uv_disconnected))
	    || (uverr = uv_signal_init(loop, &signal))
	    || (uverr = uv_signal_start(&signal, on_uv_sigint, SIGINT)))
		/* todo doesn't tear down anything */
		return thiserr(uverr, "uv init");

	uv_handle_set_data((uv_handle_t *)&poll_send, s);

	uv_run(loop, UV_RUN_DEFAULT);

	uv_loop_close(loop);
	uv_signal_stop(&signal);
	uv_poll_stop(&poll_sock);
	uv_poll_stop(&poll_send);

	return YIPPIE;
}

typedef struct {
	whl_atomic_t *whl;
	whl_efd_t    *whl_efd;
	size_t       *total;
	int           sockfd;
	uint32_t      loops;
} receiver_uv_t;

void
do_uv_read(uv_poll_t *handle, int status, int events)
{
	receiver_uv_t *r = uv_handle_get_data((uv_handle_t*)handle);
	size_t         bufsize;
	char          *buf;
	whl_offset_t   offset = whl_efd_next_shared_slice(r->whl_efd, &buf, &bufsize);

	if (offset == WHL_INVALID_OFFSET)
		return;

	if (!test_buf(buf, bufsize))
		eprintln("% 6i %x failed cmp", r->loops, offset);

	whl_efd_return_slice(r->whl_efd, offset);

	*r->total += bufsize;
	r->loops--;
}

err_t
run_uv_receiver(receiver_uv_t *r)
{
	int         uverr;
	uv_loop_t  *loop;
	uv_signal_t signal;
	/* polling on the socket is probably a good idea in general as a way
	 * to get notified of when the other end is no longer capable of
	 * communicating */
	uv_poll_t   poll_sock;
	uv_poll_t   poll_read;

	if (!(loop = uv_default_loop()))
		return thiserr(ENOMEM, "uv_default_loop");

	if (   (uverr = uv_poll_init(loop, &poll_read, r->whl_efd->readable))
	    || (uverr = uv_poll_start(&poll_read, UV_READABLE, do_uv_read))
	    || (uverr = uv_poll_init(loop, &poll_sock, r->sockfd))
	    || (uverr = uv_poll_start(&poll_sock, UV_DISCONNECT, on_uv_disconnected))
	    || (uverr = uv_signal_init(loop, &signal))
	    || (uverr = uv_signal_start(&signal, on_uv_sigint, SIGINT)))
		/* todo doesn't tear down anything */
		return thiserr(uverr, "uv init");

	uv_handle_set_data((uv_handle_t *)&poll_read, r);

	uv_run(loop, UV_RUN_DEFAULT);

	uv_loop_close(loop);
	uv_signal_stop(&signal);
	uv_poll_stop(&poll_sock);
	uv_poll_stop(&poll_read);

	return YIPPIE;
}

#endif // WITH_LIBUV

void
run_spin_sender(whl_t *whl, size_t *total)
{
	xorshiftr128plus_t rng = rng_init;
	uint32_t      loops = NLOOPS;
	whl_offset_t  offset;
	char         *buf;
	size_t        bufsize;

	while (loops--) {
		bufsize = xorshiftr128plus(&rng) % SEND_SIZE_MAX;

		/* spin */
		while ((offset = whl_make_slice(whl, &buf, bufsize)) == WHL_INVALID_OFFSET);

		write_buf(buf, bufsize);

		whl_share_slice(whl, offset);

		*total += bufsize;
	}
}

err_t
_main_sender_libuv(int sockfd, size_t *total)
{
#if WITH_LIBUV
	err_t         e = YIPPIE;
	int           memfd;
	whl_atomic_t *whl;
	whl_efd_t     whl_efd;

	if (iserr(e = open_memfd(&memfd)))
		return e;

	/* memfd is open */

	if (iserr(e = open_shm(memfd, (char **)&whl))) {
		close(memfd);
		return e;
	}

	/* shm is open */

	if (   whl_atomic_init(whl, WHEEL_SIZE) < 0
	    || whl_efd_init(&whl_efd, whl) < 0) {
		e = err("whl_efd_init");
		close_shm((char *)whl);
		close(memfd);
		return e;
	}

	/* whl_efd is open */

	int fds[] = { memfd, -1, -1 };
	whl_efd_fds(&whl_efd, &fds[1], &fds[2]);
	if (send_fds(sockfd, fds, nelements(fds)) < 0) {
		e = err("send_fds");
		whl_efd_close(&whl_efd);
		close_shm((char *)whl);
		close(memfd);
		return e;
	}

	close(memfd);

	eprintln("tx whl_atomic_t %p", whl);

	sender_uv_t s = {
		.whl = whl,
		.whl_efd = &whl_efd,
		.sockfd = sockfd,
		.total = total,
		.rng = rng_init,
		.loops = NLOOPS,
	};

	e = run_uv_sender(&s);

	whl_efd_close(&whl_efd);
	close_shm((char *)whl);

	return e;
#else
	return err("libuv not compiled in");
#endif
}

err_t
_main_sender_spin(int sockfd, size_t *total)
{
	err_t e = YIPPIE;
	int   memfd;
	whl_t *whl;

	if (iserr(e = open_memfd(&memfd)))
		return e;

	/* memfd is open */

	if (iserr(e = open_shm(memfd, (char **)&whl))) {
		close(memfd);
		return e;
	}

	/* shm is open */

	if (   (whl_init(whl, WHEEL_SIZE) < 0 && iserr(e = err("whl_init")))
	    || (send_fd(sockfd, memfd) < 0 && iserr(e = err("send_fd")))) {
		close_shm((char *)whl);
		close(memfd);
		return e;
	}

	close(memfd);

	eprintln("tx whl_t %p", whl);

	run_spin_sender(whl, total);

	close_shm((char *)whl);

	return e;
}

err_t
_main_sender_seqpacket(int sockfd, size_t *total)
{
	xorshiftr128plus_t rng = rng_init;
	char     buf[SEND_SIZE_MAX] = { MAGIC };
	uint32_t loops = NLOOPS;

	eprintln("tx seqpacket %i", sockfd);

	while (loops--) {
		size_t bufsize = xorshiftr128plus(&rng) % SEND_SIZE_MAX;

		if (send(sockfd, buf, bufsize, 0) < 0)
			return err("send");

		*total += bufsize;
	}

	return YIPPIE;
}

err_t
main_sender(int sockfd, tport_t tport)
{
	err_t  e;
	size_t total = 0;

	if (tport == TPORT_LIBUV)
		e = _main_sender_libuv(sockfd, &total);
	else if (tport == TPORT_SPIN)
		e = _main_sender_spin(sockfd, &total);
	else if (tport == TPORT_SEQPACKET)
		e = _main_sender_seqpacket(sockfd, &total);
	else
		return thiserr(EINVAL, "unexpected transport");

	eprintln("tx done %.3fmb", (float)total / 1024. / 1024.);

	return e;
}

void
run_spin_receiver(whl_t *whl, size_t *total)
{
	uint32_t      loops = NLOOPS;
	whl_offset_t  offset;
	char         *buf;
	size_t        bufsize;

	while (loops--) {
		/* spin */
		while ((offset = whl_next_shared_slice(whl, &buf, &bufsize)) == WHL_INVALID_OFFSET);

		if (!test_buf(buf, bufsize))
			eprintln("% 6i %x failed cmp", loops, offset);

		whl_return_slice(whl, offset);

		*total += bufsize;
	}
}

err_t
_main_receiver_libuv(int sockfd, size_t *total)
{
#ifdef WITH_LIBUV
	union { int a[3]; struct { int mem, read, write; }; } fds;

	err_t         e = YIPPIE;
	whl_atomic_t *whl;
	whl_efd_t     whl_efd;
	size_t        fds_len = nelements(fds.a);

	if (   recv_fds(sockfd, fds.a, &fds_len) < 0
	    || fds_len != nelements(fds.a))
		return err("recv_fds");

	if (iserr(e = open_shm(fds.mem, (char **)&whl))) {
		close(fds.mem);
		return e;
	}

	eprintln("rx whl_atomic_t %p", whl);

	whl_efd_init_from_eventfds(&whl_efd, whl, fds.read, fds.write);

	receiver_uv_t r = {
		.whl = whl,
		.whl_efd = &whl_efd,
		.sockfd = sockfd,
		.total = total,
		.loops = NLOOPS,
	};

	e = run_uv_receiver(&r);

	whl_efd_close(&whl_efd);
	close(fds.mem);

	return YIPPIE;
#else
	return err("libuv not compiled in");
#endif
}

err_t
_main_receiver_spin(int sockfd, size_t *total)
{
	err_t   e = YIPPIE;
	int     memfd;
	whl_t  *whl;

	if (recv_fd(sockfd, &memfd) < 0)
		return err("recv_fd");

	if (iserr(e = open_shm(memfd, (char **)&whl))) {
		close(memfd);
		return e;
	}

	eprintln("rx whl_t %p", whl);

	run_spin_receiver(whl, total);

	return YIPPIE;
}

err_t
_main_receiver_seqpacket(int sockfd, size_t *total)
{
	size_t   bufsize;
	char     buf[SEND_SIZE_MAX];
	uint32_t loops = NLOOPS;

	eprintln("rx seqpacket %i", sockfd);

	while (loops--) {
		if ((bufsize = recv(sockfd, buf, SEND_SIZE_MAX, 0)) < 0)
			return err("recv");

		if (!test_buf(buf, bufsize))
			eprintln("% 6i failed cmp", loops);

		*total += bufsize;
	}

	return YIPPIE;
}

err_t
main_receiver(int sockfd, tport_t tport)
{
	err_t      e;
	size_t     total = 0;
	timespec_t before, after;
	double     elapsed;

	clock_gettime(CLOCK_MONOTONIC, &before);

	if (tport == TPORT_LIBUV)
		e = _main_receiver_libuv(sockfd, &total);
	else if (tport == TPORT_SPIN)
		e = _main_receiver_spin(sockfd, &total);
	else if (tport == TPORT_SEQPACKET)
		e = _main_receiver_seqpacket(sockfd, &total);
	else
		return thiserr(EINVAL, "unexpected transport");

	clock_gettime(CLOCK_MONOTONIC, &after);

	elapsed = (double)(after.tv_sec - before.tv_sec)
	        + (double)(after.tv_nsec - before.tv_nsec) / (double)NANOS_PER_SEC;
	println("%f", elapsed);

	eprintln("rx done %.3fmb", (float)total / 1024. / 1024.);

	return e;
}

/* This whole thing is way easier with just fork. And technically that creates
 * a new virtual memory address space. But in practice, both mmaps would return
 * the same pointer and it wouldn't really demonstrate this working with
 * different virtual address spaces. (I tried hinting at what address to use
 * with the first argument to mmap but it didn't seem to do anything, I don't
 * know how any of that works to be honest.) */
err_t
_forking_main(char *exe, char *mode)
{
	err_t  e;
	int    sockpair[2];
	pid_t  pida;
	pid_t  pidb;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockpair) < 0)
		return err("socketpair");

	if (   ((pida = fork()) < 0 && iserr(e = err("fork")))
	    /* we have two different file descriptor spaces now */
	    /* duplicate each end to a "well-known" fd */
	    || ((dup2(sockpair[pida == 0], 69) < 0) && iserr(e = err("dup2")))) {
		close(sockpair[0]);
		close(sockpair[1]);
		return e;
	}

	close(sockpair[0]);
	close(sockpair[1]);

	/* sockpair is closed, either end is open at fd 69 in
	 * each process */

	/* fork the parent once more into the other end */

	if (pida && (pidb = fork()) < 0) {
		e = err("fork");
		close(69);
		return e;
	}

	if (pida && pidb) {
		/* parent */
		close(69);
		waitpid(pida, NULL, 0);
		waitpid(pidb, NULL, 0);
	} else {
		/* either sender or receiver branch */
		char *args[] = { exe, mode, pida ? "rx" : "tx", "69", 0 };
		if (execve(exe, args, NULL) < 0)
			return err("execve");
	}

	return YIPPIE;
}

/*
uint64_t
u64_from_str(char *s)
{
	uint64_t v;
	char     unit;

	if (sscanf(s, "%lu%c", &v, &unit) < 0)
		return ~0;

	switch (unit) {
		case 'g': case 'G': v *= 1024;
		case 'm': case 'M': v *= 1024;
		case 'k': case 'K': v *= 1024;
	}

	return v;
}
*/

tport_t
tport_from_str(char *s)
{
	if (strcmp(s, "uv") == 0)
		return TPORT_LIBUV;
	else if (strcmp(s, "spin") == 0)
		return TPORT_SPIN;
	else if (strcmp(s, "seqpacket") == 0)
		return TPORT_SEQPACKET;
	else
		return __TPORT_COUNT;
}

int
main(int argc, char *argv[])
{
	err_t e = YIPPIE;

	switch (argc) {
		case 1:
			e = _forking_main(argv[0], "uv");
			break;
		case 2:
			/* tport */
			if (!(tport_from_str(argv[1]) < __TPORT_COUNT))
				goto usage;
			e = _forking_main(argv[0], argv[1]);
			break;
		case 4:
			/* tport rx|tx fd */
			if (strcmp(argv[2], "tx") == 0) {
				e = main_sender(atoi(argv[3]), tport_from_str(argv[1]));
				break;
			} else if (strcmp(argv[2], "rx") == 0) {
				e = main_receiver(atoi(argv[3]), tport_from_str(argv[1]));
				break;
			}
		default:
		usage:
			eprintln("usage: %s [<uv|spin|seqpacket> [<rx|tx> <fd>]]", argv[0]);
			return 1;
	}

	if (iserr(e)) {
		eprintln("fatal! " ERRFMT, errfmtargs(e));
		return 1;
	}

	return 0;
}
