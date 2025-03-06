size_t
send_fds_with_data(const int sockfd,
                   const int *fds, const size_t nfds,
                   void *buf, const size_t buflen);
size_t
send_fds(const int sockfd, const int *fds, const size_t nfds);

size_t
send_fd(const int sockfd, const int fd);

size_t
recv_fds_with_data(const int sockfd,
                   int *outfds, size_t *n_outfds,
                   void *buf, size_t buflen);

size_t
recv_fds(const int sockfd, int *outfds, size_t *n_outfds);

size_t
recv_fd(const int sockfd, int* outfd);
