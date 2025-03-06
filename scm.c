#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "scm.h"

/* note that n_outfds is a count, or length, not a size of bytes */
size_t
send_fds_with_data(const int sockfd,
                   const int *fds, const size_t nfds,
                   void *buf, const size_t buflen)
{
/* there is a limit in the kernel of like 253 or something */
#define SCM_MAX_FDS 16

	assert(nfds <= SCM_MAX_FDS);

	if (nfds > SCM_MAX_FDS) {
		errno = E2BIG;
		return -1;
	}

	union {
		char           buf[CMSG_SPACE(sizeof(int) * SCM_MAX_FDS)];
		struct cmsghdr align;
	} scm;

	struct iovec iov = { .iov_base = buf, .iov_len = buflen };
	struct msghdr msg = { .msg_iov = &iov,
	                      .msg_iovlen = 1,
	                      .msg_control = scm.buf,
	                      .msg_controllen = CMSG_SPACE(sizeof(int) * nfds) };

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	*cmsg = (struct cmsghdr){ .cmsg_level = SOL_SOCKET,
	                          .cmsg_type = SCM_RIGHTS,
	                          .cmsg_len = CMSG_LEN(sizeof(int) * nfds) };
	memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfds);

	return sendmsg(sockfd, &msg, 0);
}

size_t
send_fds(const int sockfd, const int *fds, const size_t nfds)
{
	char data = '?';
	return send_fds_with_data(sockfd, fds, nfds, (void *)&data, 1);
}

size_t
send_fd(const int sockfd, const int fd)
{
	return send_fds(sockfd, &fd, 1);
}

/* n_outfds is read as a maximum, on success, this function assigns the actual
 * number of file descriptors read to n_outfds
 *
 * if this receives more file descriptors than the maximnum n_outfds,
 * they are silently closed.
 *
 * note that n_outfds is a count, or length, not a size of bytes */
size_t
recv_fds_with_data(const int sockfd,
                   int *outfds, size_t *n_outfds,
                   void *buf, size_t buflen)
{
	union {
		char           buf[CMSG_SPACE(sizeof(int) * SCM_MAX_FDS)];
		struct cmsghdr align;
	} scm;

	struct iovec iov = { .iov_base = buf, .iov_len = buflen };
	struct msghdr msg = { .msg_iov = &iov,
	                      .msg_iovlen = 1,
	                      .msg_control = scm.buf,
	                      .msg_controllen = sizeof(scm.buf) };

	int maxfds = n_outfds ? *n_outfds : 0;
	int ret;

	do {
		ret = recvmsg(sockfd, &msg, 0);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0)
		return ret;

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

	if (   cmsg == NULL
	    || cmsg->cmsg_level != SOL_SOCKET
	    || cmsg->cmsg_type != SCM_RIGHTS) {
		/* fixme set errno? */
		*n_outfds = 0;
		return ret;
	}

	int i = 0;
	int *cmsgfds = (int *)CMSG_DATA(cmsg);
	int *cmsgend = (int *)((char *)cmsg + cmsg->cmsg_len);

	while (i < maxfds && cmsgfds < cmsgend)
		outfds[i++] = *(cmsgfds++);

	if (n_outfds)
		*n_outfds = i;

	/* i guess this can happen if we received more file descriptors than we
	 * wanted, so close them? */
	if (cmsgfds != cmsgend) {
		int no_clobber = errno;
		while (cmsgfds < cmsgend)
			close(*(cmsgfds++));
		errno = no_clobber;
	}

	assert(cmsgfds == cmsgend);

	return ret;
}

size_t
recv_fds(const int sockfd, int *outfds, size_t *n_outfds)
{
	return recv_fds_with_data(sockfd, outfds, n_outfds, NULL, 0);
}

size_t
recv_fd(const int sockfd, int* outfd)
{
	size_t fds = 1;
	if (recv_fds(sockfd, outfd, &fds) < 0)
		return -1;
	return fds;
}
