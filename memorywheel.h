/* single-producer single-consumer queue for variable-sized messages
 * in a fixed-sized shared memory buffer for processes with different
 * virtual memory mappings and file descriptor tables.
 *
 * writer:
 * - `whl_make_slice()` finds a free slice of the requested size
 * - `whl_share_slice()` makes that slice gettable in the next step
 *
 * reader:
 * - `whl_next_shared_slice()` gets the earliest shared slice
 * - `whl_return_slice()` makes the slice available to the first step
 *
 * initializion:
 * - initialize the header in allocated shared memory at the start of
 *   the buffer reserved for the memory wheel. use either:
 * 1. `whl_init()` to spin on it.
 *    Initialize in only one process and cast in the other.
 * 2. `whl_atomic_init()` to poll on file descriptors.
 *    Similarly, use `whl_atomic_init()` in one process and cast in the other.
 *    But, also use `whl_efd_init()` in non-shared memory to create file
 *    descriptors in one process, access them with `whl_efd_fds()`, duplicate
 *    them to another process having a different file descriptor table, and use
 *    `whl_efd_init_from_eventfds()` there. */
#include <assert.h>
#include <stdatomic.h>
#include <sys/eventfd.h>

typedef uint8_t   u8;
typedef uint32_t  u32;
typedef char      byte;
typedef uint32_t  whl_offset_t;

#define WHL_INVALID_OFFSET      UINT32_MAX
#define WHL_INVALID_OFFSET_PAIR UINT64_MAX
/* 64 is a reasonable guess for cache line size?
 * also 64*UINT32_MAX allows for like 250GBish */
#define WHL_ALIGN               64

#define __whl_affirm(c)   while (!(c)) __builtin_unreachable()
#define __whl_staticassert(desc, test) \
	const char __static_assert ## desc [(test) ? 0 : -1]

typedef enum {
	WHL_SLICE_UNINIT   = 0x0,
	WHL_SLICE_READABLE = 0x1,
	WHL_SLICE_RETURNED = 0x2,
} whl_slice_state_e;

typedef struct whl_slice {
	/* the size in bytes the user requested, at least this many bytes is
	 * reserved for this slice in the memory immediately following the slice's
	 * address */
	size_t               trailing_user_size;
	/* WHL_ALIGN * aligned_size_in_wheel >= trailing_user_size */
	_Atomic whl_offset_t aligned_size_in_wheel;
	_Atomic u8           state;
} whl_slice_t;

typedef union {
	struct {
		whl_offset_t head;
		whl_offset_t last;
	};
	uint64_t         u64;
} whl_offset_pair_t;

const whl_offset_pair_t whl_invalid_offset_pair =
	{ .u64 = WHL_INVALID_OFFSET_PAIR };

/* lives in shared memory */
typedef struct {
	/* the size of the usable buffer in memory following */
	whl_offset_t aligned_size;
	union {
		struct {
			/* head is the oldest slice that is "allocated" and not returned,
			 * or WHL_INVALID_OFFSET if no such slice */
			_Atomic whl_offset_t head;
			/* last is the most recent slice "allocated" and not returned,
			 * or WHL_INVALID_OFFSET if no such slice */
			_Atomic whl_offset_t last;
		};
		_Atomic whl_offset_pair_t head_last;
	};
} whl_spin_t;

/* lives in shared memory */
typedef struct {
	whl_spin_t   spin;
	/* initially 0. set to 1 when a slice is shared. */
	_Atomic u8   is_readable;
	/* initially 1. set to 0 when making a slice fails. */
	_Atomic u8   is_writable;
} whl_atomic_t;


/* I don't know if this is really important */
__whl_staticassert(whl_slice_t_sizeof, sizeof(whl_slice_t) == 16);
/* whl_spin_t and whl_atomic_t do need to be <= WHL_ALIGN because
 * of whl_init only reserves WHL_ALIGN room for the wheel header.
 * it would almost be fine to accept a variable size and pad it
 * but __whl_buf() would have to change */
__whl_staticassert(whl_spin_t_sizeof, sizeof(whl_spin_t) <= WHL_ALIGN);
__whl_staticassert(whl_aotmic_t_sizeof, sizeof(whl_atomic_t) <= WHL_ALIGN);

/* a copy for each process with different file descriptor table or virtual
 * address space */
typedef struct {
	whl_atomic_t *atomic;
	/* a file descriptor that polls readable when at least one message is
	 * shared and not yet taken */
	int           readable;
	/* a file descriptor that polls writable when there might be room for
	 * message */
	int           writable;
} whl_efd_t;

typedef whl_spin_t whl_t;

#define __whl_buf(wheel)            ((byte *)(wheel) + WHL_ALIGN)
#define __whl_slice_buf(slice)      ((byte *)(((whl_slice_t *)(slice)) + 1))
#define __whl_alignment_padding(sz) ((WHL_ALIGN - ((sz) % WHL_ALIGN)) % WHL_ALIGN)

/* returns `size` rounded up to the nearest multiple of `WHL_ALIGN` */
size_t
whl_align(size_t size)
{
	return size + __whl_alignment_padding(size);
}

int
__whl_efd_write(int efd, uint64_t v)
{
	ssize_t r;
	do {
		r = write(efd, &v, sizeof(v));
	} while (r < 0 && errno == EINTR);
	return r == sizeof(v);
}

int
__whl_efd_read(int efd)
{
	uint64_t v;
	ssize_t  r;
	do {
		r = read(efd, &v, sizeof(v));
	} while (r < 0 && errno == EINTR);
	return r == sizeof(v);
}

/* `wheel` must point to allocated memory at least `size` big.
 * `buf_size` must be a multiple of 64, at least 128, less than 64 * u32 max.
 *
 * Returns 0 on success, non-zero on error.
 *
 * `wheel` should point to shared memory.
 *
 * This initializes whl_t at the address pointed at by `wheel`.
 * The buffer from wheel + 64 to wheel + size will be used by whl_t in
 * functions like `whl_make_slice()`. */
int
whl_init(whl_t *wheel, size_t buf_size)
{
	if (   buf_size < 2 * WHL_ALIGN
	    || buf_size % WHL_ALIGN != 0
	    || buf_size >= WHL_ALIGN * UINT32_MAX)
		return -1;

	*wheel = (whl_t) {
		.aligned_size = (buf_size - WHL_ALIGN) / WHL_ALIGN,
		.head = WHL_INVALID_OFFSET,
		.last = WHL_INVALID_OFFSET,
	};
	return 0;
}

/* See whl_init for arguments.
 *
 * Initializes a `whl_spin_t`, so use either `whl_init` or this
 * function. This is meant to be used with `whl_efd_t` and
 * `whl_efd_init`.
 *
 * `whl_atomic_t` should point to and be initialized in shared memory.
 * On the other hand, `whl_efd_t` stores file descriptors will not be
 * valid if shared between processes with different file descriptor sets.
 *
 * So one end can initialize `whl_atomic_t` but both ends initialize there
 * own `whl_efd_t`, possibly by creating new file descriptors with
 * `whl_efd_init`, duplicating them over scm_rights to another process,
 * using `whl_efd_init_from_eventfds` there.
 *
 * Don't use a `whl_spin_t` on one end with a `whl_efd_t` on the other end,
 * since the non-efd functions here won't sync or update the eventfds
 * appropriately.
 *
 * Returns 0 on success, non-zero on error. */
int
whl_atomic_init(whl_atomic_t *wheel, size_t buf_size)
{
	*wheel = (whl_atomic_t) {
		.is_readable = 0,
		.is_writable = 1,
	};
	return whl_init(&wheel->spin, buf_size);
}

/* Initializes `whl_efd_t` using the given already initialized `whl_atomic_t`
 * and two eventfd file descriptors. */
void
whl_efd_init_from_eventfds(whl_efd_t *wheel,
                           whl_atomic_t *atomic,
                           int readable,
                           int writable)
{
	*wheel = (whl_efd_t) {
		.atomic = atomic,
		.readable = readable,
		.writable = writable,
	};
}

/* Initializes `whl_efd_t` using the given already initialized `whl_atomic_t`.
 *
 * Creates eventfds for polling with an event loop. These file descriptors
 * should be duplicated (via scm_rights or something) to any process that might
 * use the same memory wheel from a different file descriptor mapping.
 *
 * Use `whl_efd_init_from_eventfds` to Initializes `whl_efd_t` from existing
 * file descriptors.
 *
 * eventfds are created with EFD_NONBLOCK | EFD_CLOEXEC
 *
 * Returns 0 on success, non-zero on error.
 * errno is probably set from the underlying failed eventfd call. */
int
whl_efd_init(whl_efd_t *wheel, whl_atomic_t *atomic)
{
	/* Reasoning for EFD_SEMAPHORE
	 *
	 * Consider a reader that finds no readable item.
	 *
	 * R1: If is_readable newly becomes zero,
	 *     if (1 == atomic_exchange(&is_readable, 0))
	 * R2: then ensure the eventfd to not readable.
	 *         read(readable_fd)
	 *
	 * Also consider a writer that just shared a slice.
	 *
	 * W1: If is_readable newly becomes non-zero,
	 *     if (0 == atomic_exchange(&is_readable, 1))
	 * W2: then ensure the eventfd is readable.
	 *         write(readable_fd, 0)
	 *
	 * It's possible to perform R1 W1 W2 R2. Without EFD_SEMAPHORE,
	 * this leaves the atomic is_readable at 1 (because W1 followed R1)
	 * but the eventfd non-readable (because R2 followed W2).
	 *
	 * EFD_SEMAPHORE will accumulate the operations of both W2 and R2
	 * in any order. */

	int flags = EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE;
	int readable;
	int writable;

	if ((readable = eventfd(atomic->is_readable, flags)) < 0) {
		return -1;
	}

	if ((writable = eventfd(0, flags)) < 0) {
		int no_clobber = errno;
		close(readable);
		errno = no_clobber;
		return -1;
	}

	/* we can't initialize eventfd with this value because, even though
	 * internally it uses a 64-bit value, the parameter to eventfd is like
	 * 32-bits or something; very epic */

	if (__whl_efd_write(writable, ~0 - 1 - atomic->is_writable) < 0) {
		int no_clobber = errno;
		close(writable);
		close(readable);
		errno = no_clobber;
		return -1;
	}

	whl_efd_init_from_eventfds(wheel, atomic, readable, writable);
	return 0;
}

/* closes the two eventfd file descriptors */
void
whl_efd_close(whl_efd_t *wheel)
{
	close(wheel->readable);
	close(wheel->writable);
}

/* copies file descriptors in order corresponding to
 * `whl_efd_init_from_eventfds`
 *
 * does not dupilcate file descriptors, just copies their value
 *
 * literally just so you don't have to worry about getting the
 * parameter sequence right */
void
whl_efd_fds(whl_efd_t *wheel, int *readable, int *writable)
{
	*readable = wheel->readable;
	*writable = wheel->writable;
}

whl_slice_t *
__whl_at_unchecked(whl_t *wheel, whl_offset_t offset)
{
	return (whl_slice_t *)(__whl_buf(wheel) + WHL_ALIGN * offset);
}

whl_slice_t *
__whl_head(whl_t *wheel)
{
	whl_offset_t o = atomic_load(&wheel->head);
	if (o == WHL_INVALID_OFFSET)
		return NULL;
	else
		return __whl_at_unchecked(wheel, o);
}

whl_slice_t *
__whl_last(whl_t *wheel)
{
	whl_offset_t o = atomic_load(&wheel->last);
	if (o == WHL_INVALID_OFFSET)
		return NULL;
	else
		return __whl_at_unchecked(wheel, o);
}

whl_offset_t
__whl_next_offset_aligned(whl_t *wheel, whl_offset_t size,
                          whl_offset_pair_t pair)
{
	/* single-producer single-consumer
	 *
	 * - if head <= last, it will either, stay head <= last
	 *   OR will be set to WHL_INVALID_OFFSET
	 * - if head > last, we're wrapped around, head could advance or
	 *   wrap around and follow behaviour as above */

	if (pair.u64 == WHL_INVALID_OFFSET_PAIR) {

		if (size <= wheel->aligned_size)
			return 0;

	} else {
		whl_offset_t head = pair.head;
		whl_offset_t last = pair.last;
		whl_offset_t last_end =
			last + __whl_at_unchecked(wheel, last)->aligned_size_in_wheel;

		__whl_affirm(head != WHL_INVALID_OFFSET);
		__whl_affirm(last != WHL_INVALID_OFFSET);

		if (last < head) {

			/* We've wrapped around, so we can only use area from the end of
			 * the last slice up to the start of the first */
			if (size <= head - last_end)
				return last_end;

		} else {

			/* Try after the end of the last slice, not past the wheel end */
			if (size <= wheel->aligned_size - last_end)
				return last_end;

			/* Or maybe wrap around from the wheel start until the head */
			/* TODO, if we do this, we have to backfill FIXME */
			if (size <= head)
				return 0;

		}

	}

	return WHL_INVALID_OFFSET;
}

/* on success, copies the buf pointer to *bufp and returns an offset
 *
 * if there isn't room for a slice of this size,
 * returns WHL_INVALID_OFFSET and *bufp is untouched */
whl_offset_t
whl_make_slice(whl_t *wheel, byte **bufp, size_t size)
{
	/* guard from overflow */
	if (size > SIZE_MAX - sizeof(whl_slice_t))
		return WHL_INVALID_OFFSET;

	whl_offset_t  offset;
	size_t        size_in_wheel = sizeof(whl_slice_t) + size;

	size_in_wheel += __whl_alignment_padding(size_in_wheel);
	__whl_affirm(size_in_wheel % WHL_ALIGN == 0);

	whl_offset_t      aligned_size_in_wheel = size_in_wheel / WHL_ALIGN;
	whl_offset_pair_t pair = atomic_load(&wheel->head_last);

	if ((offset = __whl_next_offset_aligned(wheel, aligned_size_in_wheel, pair)) == WHL_INVALID_OFFSET)
		return WHL_INVALID_OFFSET;

	whl_offset_t old_last = pair.last;

	/* backfill,
	 * there cannot be a void after the (old) last slice,
	 * else we can't return it
	 *
	 * =( ------[slice]------|
	 * =D ------[slice~~~~~~]|
	 *
	 * FIXME we can probably shrink the slice struct if we smuggle this into
	 * the wheel struct instead */
	if (offset == 0 && (old_last != WHL_INVALID_OFFSET))
		atomic_store(&__whl_at_unchecked(wheel, old_last)->aligned_size_in_wheel,
		             wheel->aligned_size - old_last);

	*__whl_at_unchecked(wheel, offset) = (whl_slice_t) {
		.trailing_user_size = size,
		.aligned_size_in_wheel = aligned_size_in_wheel,
	};

	*bufp = __whl_slice_buf(__whl_at_unchecked(wheel, offset));

	/* below is basically just an atomic version of
	 *
	 * wheel->last = offset;
	 * if (wheel->head == WHL_INVALID_OFFSET)
	 *     wheel->head = offset; */
	do {
		/* invariant:
		 * head and last must always be either both valid or both invalid */
		if (pair.u64 == WHL_INVALID_OFFSET_PAIR) {
			/* if head was invalid, it will remain invalid
			 * because this is single-producer single-consumer and the consumer
			 * does not move head off from the invalid offset */
			pair = (whl_offset_pair_t) { .head = offset, .last = offset };
			atomic_store(&wheel->head_last, pair);
			break;
		} else {
			/* head was not invalid, it _could_ have become so since we last
			 * saw it, so compare and exchange to keep the invariant */
			whl_offset_pair_t new_pair = { .head = pair.head, .last = offset };
			if (atomic_compare_exchange_strong(&wheel->head_last,
			                                   /* expected */
			                                   &pair,
			                                   /* desired */
			                                   new_pair))
				break;
		}
		pair = atomic_load(&wheel->head_last);
	} while (1);

	return offset;
}

/* `whl_efd_t` version of `whl_make_slice`
 *
 * if this returns WHL_INVALID_OFFSET it will try to set
 * `whl_efd_t` `writable` to unwritable when polled. if that fails,
 * errno will be non-zero.
 *
 * As a warning, if this returns WHL_INVALID_OFFSET while the queue is empty,
 * it will become unreadable and unwritable. This can happen if you try to take
 * a slice that is larger than the buffer supports. */
whl_offset_t
whl_efd_make_slice(whl_efd_t *wheel, byte **bufp, size_t size)
{
	whl_offset_t offset = whl_make_slice(&wheel->atomic->spin, bufp, size);

	/* clear errno, it may be set by __whl_efd_write */
	errno = 0;

	if (   offset == WHL_INVALID_OFFSET
	    && 1 == atomic_exchange(&wheel->atomic->is_writable, 0))
		__whl_efd_write(wheel->writable, 1);

	return offset;
}

/* called after `whl_make_slice` to make a slice available to be returned by
 * `whl_next_shared_slice` by another process */
void
whl_share_slice(whl_t *wheel, whl_offset_t offset)
{
	atomic_store(&__whl_at_unchecked(wheel, offset)->state,
	             WHL_SLICE_READABLE);
}

/* `whl_efd_t` version of `whl_share_slice`
 *
 * may try to set `whl_efd_t` `readable` to readable when polled.
 * if that fails, errno will be non-zero. */
void
whl_efd_share_slice(whl_efd_t *wheel, whl_offset_t offset)
{
	whl_share_slice(&wheel->atomic->spin, offset);

	/* clear errno, it may be set by __whl_efd_write */
	errno = 0;

	if (0 == atomic_exchange(&wheel->atomic->is_readable, 1))
		__whl_efd_write(wheel->readable, 1);
}

/* this does not advance the read head, calling this again will return the same
 * slice, return the previous slice before calling this again.
 *
 * only modifies bufp and size on success
 * returns WHL_INVALID_OFFSET if the next slice is not shared */
whl_offset_t
whl_next_shared_slice(whl_t *wheel, byte **bufp, size_t *size)
{
	whl_offset_t offset = atomic_load(&wheel->head);

	if (offset == WHL_INVALID_OFFSET)
		return WHL_INVALID_OFFSET;

	whl_slice_t *slice = __whl_at_unchecked(wheel, offset);

	if (atomic_load(&slice->state) != WHL_SLICE_READABLE)
		return WHL_INVALID_OFFSET;

	*bufp = __whl_slice_buf(slice);
	*size = slice->trailing_user_size;
	return offset;
}

/* `whl_efd_t` version of `whl_next_shared_slice`
 *
 * if this returns WHL_INVALID_OFFSET it will try to set
 * `whl_efd_t` `readable` to unreadable when polled. if that fails,
 * errno will be non-zero. */
whl_offset_t
whl_efd_next_shared_slice(whl_efd_t *wheel, byte **bufp, size_t *size)
{
	whl_offset_t offset = whl_next_shared_slice(&wheel->atomic->spin,
	                                            bufp, size);

	/* clear errno, it may be set by __whl_efd_read */
	errno = 0;

	if (   offset == WHL_INVALID_OFFSET
	    && 1 == atomic_exchange(&wheel->atomic->is_readable, 0))
		__whl_efd_read(wheel->readable);

	return offset;
}

/* after getting a slice from `whl_next_shared_slice`, this
 * "frees" it so that it can be re-used by `whl_make_slice` */
size_t
whl_return_slice(whl_t *wheel, whl_offset_t off)
{
	/* single-producer single-consumer
	 * - last can change
	 * - head can change if it was WHL_INVALID_OFFSET */

	size_t             returns = 0;
	whl_slice_t       *slice = __whl_at_unchecked(wheel, off);
	whl_offset_pair_t  pair;

	if (WHL_SLICE_RETURNED == atomic_exchange(&slice->state, WHL_SLICE_RETURNED))
		return 0;

    /* this is supposed to handle returns in any order, like in case you pass
     * the offset from whl_make_slice in a different way than whl_share_slice
     * and whl_next_shared_slice, and then return the passed offsets in a
     * different order than they were given from whl_make_slice. mostly for
     * multi-producer multi-consumer.
     * but I don't think I ever tested it so idk lol =) */

	while (   (pair = atomic_load(&wheel->head_last)).head != WHL_INVALID_OFFSET
	       && (atomic_load(&__whl_head(wheel)->state) == WHL_SLICE_RETURNED)) {

		if (   pair.head == pair.last
		    && atomic_compare_exchange_strong(&wheel->head_last,
		                                      /* expected */
		                                      &pair,
		                                      /* desired */
		                                      whl_invalid_offset_pair)) {
			/* =) */
		} else {
			whl_slice_t *head = __whl_at_unchecked(wheel, pair.head);
			whl_offset_t next_head
				= (pair.head + atomic_load(&head->aligned_size_in_wheel))
				% wheel->aligned_size;
			atomic_store(&wheel->head, next_head);
		}

		returns++;
	}

	return returns;
}

/* `whl_efd_t` version of `whl_return_slice`
 *
 * may try to set `whl_efd_t` `writable` to writable when polled.
 * if that fails, errno will be non-zero. */
size_t
whl_efd_return_slice(whl_efd_t *wheel, whl_offset_t off)
{
	size_t r = whl_return_slice(&wheel->atomic->spin, off);

	/* clear errno, it may be set by __whl_efd_read */
	errno = 0;

	if (0 == atomic_exchange(&wheel->atomic->is_writable, 1))
		__whl_efd_read(wheel->writable);

	return r;
}
