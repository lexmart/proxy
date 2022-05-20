/*

   -Use hash table to lookup connection pointer instead of linear scan


*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <time.h>

#define arrlen(arr) ((sizeof(arr))/sizeof((arr)[0]))

#define MAXCONNS 128
#define BUFSIZE 4096

typedef struct buffer {
	uint8_t *data;
	int sent;
	int len;
} buffer;

typedef struct connection {
	uint8_t tunneled;
	int clientfd;
	int serverfd;
	uint8_t clientfd_read_ready;
	uint8_t serverfd_read_ready;
	uint8_t clientfd_write_ready;
	uint8_t serverfd_write_ready;
	buffer client_buf;
	buffer server_buf;
} connection;

uint8_t is_buf_full(buffer *buf) {
	return buf->len == BUFSIZE;
}

uint8_t is_buf_empty(buffer *buf) {
	return buf->sent == buf->len;
}

void fatal_error(char *error,...) {
	printf("Error: %s\n", error);
	exit(1);
}

int get_server_socket(char *listen_ip, char *port) {
	int sock;
	struct addrinfo hints;
	struct addrinfo *ai;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if(getaddrinfo(listen_ip, port, &hints, &ai) != 0) {
		fatal_error("getaddrinfo");
	}

	sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if(sock < 0) {
		fatal_error("socket");
	}
	if(bind(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
		fatal_error("bind");
	}
	if(listen(sock, 20) < 0) {
		fatal_error("listen");
	}
	freeaddrinfo(ai);
	return sock;
}

void add_epoll_fd(uint32_t events, int ep, int fd) {
	struct epoll_event event;
	event.events = events;
	event.data.fd = fd;
	if(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event)) {
		fatal_error("add_epoll_fd");
	}
}

uint8_t add_connection(int serverfd, int ep, connection *conns, int *conncount) {
	struct sockaddr_storage their_addr;
	socklen_t addr_size = sizeof(their_addr);
	int clientfd = accept(serverfd, (struct sockaddr *)&their_addr, &addr_size);
	if(clientfd < 0) {
		return 0;
	}
	fcntl(clientfd, F_SETFL, O_NONBLOCK);
	add_epoll_fd(EPOLLIN | EPOLLOUT | EPOLLET, ep, clientfd);

	assert((*conncount) < MAXCONNS);
	connection *conn = conns + (*conncount);
	memset(conn, 0, sizeof(connection));
	conn->clientfd = clientfd;
	conn->serverfd = -1;
	conn->client_buf.data = malloc(BUFSIZE);
	conn->server_buf.data = malloc(BUFSIZE);
	*conncount = (*conncount) + 1;
	return 1;
}

void kill_connection(connection *conns, int *conncount, int i) {
	printf("killing connection...\n");
	connection *conn = conns + i;
	close(conn->clientfd);
	close(conn->serverfd);

	(*conncount) = (*conncount) - 1;
	if((*conncount) >= 0) {
		conns[i] = conns[*conncount];
	}
}

void set_read_ready(int fd, connection *conns, int conncount) {
	for(int i = 0; i < conncount; i++) {
		connection *conn = conns + i;
		if(conn->clientfd == fd) {
			conn->clientfd_read_ready = 1;
			break;
		} else if(conn->serverfd == fd) {
			conn->serverfd_read_ready = 1;
			break;
		}
	}
}

void set_write_ready(int fd, connection *conns, int conncount) {
	for(int i = 0; i < conncount; i++) {
		connection *conn = conns + i;
		if(conn->clientfd == fd) {
			conn->clientfd_write_ready = 1;
			break;
		} else if(conn->serverfd == fd) {
			conn->serverfd_write_ready = 1;
			break;
		}
	}
}

void read_to_buffer(int *fd, buffer *buf, uint8_t *read_ready) {
	while(*read_ready && !is_buf_full(buf)) {
		int nbytes = recv(*fd, buf->data + buf->len, BUFSIZE - buf->len, 0);
		printf("read %d bytes\n", nbytes);
		if(nbytes == 0) {
			*fd = -1;
			*read_ready = 0;
		} else if(nbytes < 0) {
			*read_ready = 0;
			if(errno != EAGAIN && errno != EWOULDBLOCK) {
				*fd = -1;
			}
		} else {
			buf->len += nbytes;
		}
	}
}

void write_from_buffer(int *fd, buffer *buf, uint8_t *write_ready) {
	while(*write_ready && !is_buf_empty(buf)) {
		int nbytes = send(*fd, buf->data + buf->sent, buf->len - buf->sent, MSG_NOSIGNAL);
		printf("wrote %d bytes\n", nbytes);
		if(nbytes == 0) {
			*fd = -1;
			*write_ready = 0;
		} else if(nbytes < 0) {
			*write_ready = 0;
			if(errno != EAGAIN && errno != EWOULDBLOCK) {
				*fd = -1;
			}
		} else {
			buf->sent += nbytes;
		}
	}

	if(is_buf_empty(buf)) {
		buf->sent = 0;
		buf->len = 0;
	}
}

int find_pattern(uint8_t *buf, int buflen, uint8_t *pattern, int patternlen) {
	for(int i = 0; i < buflen - patternlen + 1; i++) {
		uint8_t match = 1;
		for(int j = 0; j < patternlen; j++) {
			if(buf[i+j] != pattern[j]) {
				match = 0;
				break;
			}
		}
		if(match) {
			return i;
		}
	}
	return -1;
}

int get_next_line(buffer *buf, int i) {
	return find_pattern(buf->data + i, buf->len - i, (uint8_t *)"\r\n", 2);
}

int main() {
	int ep = epoll_create(20);
	if(ep < 0) {
		fatal_error("epoll_create");
	}
	int serverfd = get_server_socket(0, "6969");
	add_epoll_fd(EPOLLIN, ep, serverfd);
	struct epoll_event events[32];
	connection conns[MAXCONNS];
	int conncount = 0;

	for(;;) {
		printf("waiting for poll event...\n");
		int fdcount = epoll_wait(ep, events, arrlen(events), -1);
		for(int i = 0; i < fdcount; i++) {
			printf("poll event\n");
			struct epoll_event *event = events + i;
			int sockfd = event->data.fd;
			if(event->data.fd == serverfd) {
				if(event->events == EPOLLIN) {
					uint8_t added = add_connection(serverfd, ep, conns, &conncount);
				} else {
					fprintf(stderr, "server event not EPOLLIN but %d\n", event->events);
					exit(1);
				}
			} else {
				if(event->events & EPOLLIN) {
					set_read_ready(sockfd, conns, conncount);
				}
				if(event->events & EPOLLOUT) {
					printf("\n\n\n%d write ready\n\n\n", sockfd);
					set_write_ready(sockfd, conns, conncount);
				} 
				if((events->events & (EPOLLIN|EPOLLOUT)) == 0) {
					fprintf(stderr, "server event not EPOLLIN not EPOLLOUT but %d\n", event->events);
					exit(1);
				}
			}
		}

		for(int i = 0; i < conncount; i++) {
			connection *conn = conns + i;
			if(!conn->tunneled) {
				while(conn->clientfd >= 0 && conn->clientfd_read_ready && !is_buf_full(&conn->client_buf)) {
					read_to_buffer(&conn->clientfd, &conn->client_buf, &conn->clientfd_read_ready);
				}
				if(conn->client_buf.len >= 10) {
					uint8_t method_match = 
						!strncmp((char *)conn->client_buf.data, "GET ", 4) 		|| 
						!strncmp((char *)conn->client_buf.data, "POST ", 5) 	|| 
						!strncmp((char *)conn->client_buf.data, "DELETE ", 7) 	|| 
						!strncmp((char *)conn->client_buf.data, "OPTIONS ", 8) 	|| 
						!strncmp((char *)conn->client_buf.data, "PUT ", 4);

					if(method_match) {
						uint8_t found_host = 0;
						char host[256];
						int i = get_next_line(&conn->client_buf, 0);
						while(i >= 0) {
							if(!strncmp((char *)conn->client_buf.data + i, "\r\nHost: ", 8)) {
								int hostlen = get_next_line(&conn->client_buf, i+8);
								if(hostlen < 0) {
									fatal_error("host name not ended with \\r\\n\n");
								} else if(hostlen < sizeof(host)) {
									memcpy(host, conn->client_buf.data + i + 2 + 6, hostlen);
									host[hostlen] = 0;
									found_host = 1;
									break;
								} else {
									fatal_error("host length too long");
								}
							} else {
								i += get_next_line(&conn->client_buf, i+2);
							}
						}

						if(found_host) {
							struct addrinfo hints;
							struct addrinfo *ai;
							memset(&hints, 0, sizeof(struct addrinfo));
							hints.ai_family = AF_UNSPEC;
							hints.ai_socktype= SOCK_STREAM;
							printf("host=%s\n", host);
							if(getaddrinfo(host, "80", &hints, &ai) != 0) {
								fatal_error("getaddrinfo");
							}
							int serverfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
							if(serverfd < 0) {
								fatal_error("socket");
							}
							if(connect(serverfd, ai->ai_addr, ai->ai_addrlen) != 0) {
								fatal_error("connect");
							}
							conn->serverfd = serverfd;
							conn->tunneled = 1;
							fcntl(serverfd, F_SETFL, O_NONBLOCK);
							add_epoll_fd(EPOLLIN | EPOLLOUT | EPOLLET, ep, serverfd);
						} else {
							fatal_error("did not find host");
						}
					} else {
						fatal_error("not a valid method");
					}
				}
			}
		}

		for(int i = 0; i < conncount; i++) {
			uint8_t keep_going;
			do {
				keep_going = 0;
				connection *conn = conns + i;
				if(conn->clientfd >= 0) read_to_buffer(&conn->clientfd, &conn->client_buf, &conn->clientfd_read_ready);
				if(conn->serverfd >= 0) write_from_buffer(&conn->serverfd, &conn->client_buf, &conn->serverfd_write_ready);
				printf("reading from server...\n");
				if(conn->serverfd >= 0) read_to_buffer(&conn->serverfd, &conn->server_buf, &conn->serverfd_read_ready);
				printf("clientfd = %d client write ready = %d\n", conn->clientfd, conn->clientfd_write_ready);
				if(conn->clientfd >= 0) write_from_buffer(&conn->clientfd, &conn->server_buf, &conn->clientfd_write_ready);

				keep_going = 
					(conn->serverfd_write_ready && !is_buf_empty(&conn->client_buf)) ||
					(conn->clientfd_write_ready && !is_buf_empty(&conn->server_buf)) ||
					(conn->serverfd_read_ready && !is_buf_full(&conn->server_buf)) ||
					(conn->clientfd_read_ready && !is_buf_full(&conn->client_buf));

			} while(keep_going);
		}	

		for(int i = 0; i < conncount;) {
			connection *conn = conns + i;
			uint8_t serverfd_closed = conn->serverfd < 0;
			uint8_t clientfd_closed = conn->clientfd < 0;
			uint8_t keep_going = 
				(!serverfd_closed && !clientfd_closed) ||
				(!conn->tunneled && !clientfd_closed) ||
				(!serverfd_closed && !is_buf_empty(&conn->client_buf)) ||
				(!clientfd_closed && !is_buf_empty(&conn->server_buf));

			if(!keep_going) {
				kill_connection(conns, &conncount, i);
			} else {
				i++;
			}
		}
	}
}



















