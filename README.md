## Just use sockets. They're great!

`memorywheel.h` has a single-producer single-consumer queue for variable-sized
messages in a fixed-sized shared memory buffer for processes with different
virtual memory mappings and file descriptor tables.

I also haven't tested this much so it might be buggy.

This is [CC0](https://creativecommons.org/publicdomain/zero/1.0/). But if you
use it for evil (you know who you are) then don't be surprised if all your
chakras fall out of your butt and you have a bad time.

## You don't need this.

Most of the time when you want a thread-safe queue, you are using it in one
process with threads.

That process shares virtual memory space. So if one thread passes a pointer to
another thread, that thread can read it because it shares memory _and_ that
memory has the same address so the same pointers point to the same place in
both threads.

The threads also shares file descriptor tables so if one thread creates a file
descriptor another thread can interact with it trivially.

`memorywheel.h` is for processes that don't share virtual memory spaces or file
descriptor tables. It uses relative offsets instead of pointers. And expects
each process will have its own file descriptors to whatever underlying resource.

## example.c

There's an example in `example.c`. It uses `memfd_create()` and
`SOCK_SEQPACKET` so I don't know how portable it is.

It has three modes of operation:

1. Spin to queue and dequeue messages as in a loop.
2. Use libuv to queue and dequeue messages when the memorywheel is writable or
   readable.
3. Don't use memorywheel and send messages over SOCK_SEQPACKET.

Mode two requires libuv to be linked in. It's enabled by default in the 
build.ninja file but can be built without it by not defining `WITH_LIBUV`.

## timings

The difference in performance varies dramatically based on the parameters of
the test. More details on that after the results.

This is how the program runs on my computer. It uses two processes to do 1M
sends with a random message size between 0 and 32k. The sender writes to the
entire buffer, the reader only reads the first few bytes.

    > time ./build/example spin
    tx whl_t 0x7f59162f9000
    rx whl_t 0x7fabb485a000
    rx done 15636.845mb
    tx done 15636.845mb

    ________________________________________________________
    Executed in  842.83 millis    fish           external
       usr time    1.58 secs      0.00 millis    1.58 secs
       sys time    0.07 secs      3.88 millis    0.06 secs

    > time ./build/example uv
    tx whl_atomic_t 0x7fe2640ef000
    rx whl_atomic_t 0x7f383c93e000
    tx done 15636.859mb
    rx done 15636.859mb

    ________________________________________________________
    Executed in    1.38 secs    fish           external
       usr time    1.63 secs    0.18 millis    1.63 secs
       sys time    1.11 secs    3.92 millis    1.10 secs

    > time ./build/example seqpacket
    rx seqpacket 69
    tx seqpacket 69
    tx done 15636.845mb
    rx done 15636.845mb

    ________________________________________________________
    Executed in    2.77 secs    fish           external
       usr time    0.70 secs    0.00 millis    0.70 secs
       sys time    4.65 secs    4.51 millis    4.64 secs

As far as I can tell, the spin stuff performs badly if one end is
disproportionately slower than another end, such that one end spins more than
the other. My understanding is that spinning is surprisingly complicated to do
well and performant spin locks are difficult.

In this program on my computer, spinning can be about ten times as fast as the
version that uses eventfd file descriptors with libuv. But as I bump up the
maximum message size to like 512k, the writer becomes slower than the reader,
and spinning becomes slower than polling.

The seqpacket version sends over a unix SOCK_SEQPACKET socket and slows down as
the message size increases, presumably because the copy from user to kernel
takes more time.

But the cost of any syscall is pretty high. The libuv test makes a syscall
every time the memorywheel changes between full and non-full or empty and
non-empty. A lot of the cost is from that. In fact, if the memorywheel were to
modify file descriptors on every queue or unqueue, it's about as slow or slower
than a socket except for really big packets.

So just use sockets. They're great! And I wouldn't be surprised if sockets with
io_uring is faster than this anyway. =)
