/*

   	- Rewrite first packet HOST packet code. It's pretty TRASH.

   	- Use hash table to lookup connection pointer instead of linear scan

	- local ip can change, get based on MAC address? http://www.microhowto.info/howto/get_the_ip_address_of_a_network_interface_in_c_using_siocgifaddr.html


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
	int reuse = 1;
	if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int)) != 0) {
		fatal_error("setsockopt");
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

void free_buffer(intptr_t **freebufs, uint8_t *buf) {
	*(intptr_t *)buf = (intptr_t)*freebufs;
	*freebufs = (intptr_t *)buf;
}

uint8_t *get_buffer(intptr_t **freebufs) {
	uint8_t *result = 0;
	if(*freebufs) {
		result = (uint8_t *)*freebufs;
		*freebufs = (intptr_t *)**freebufs;
	} else {
		result = malloc(BUFSIZE);
	}

	return result;
}

uint8_t add_connection(int serverfd, int ep, connection *conns, int *conncount, intptr_t **freebufs) {
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
	conn->client_buf.data = get_buffer(freebufs);
	conn->server_buf.data = get_buffer(freebufs);
	*conncount = (*conncount) + 1;
	return 1;
}

void kill_connection(connection *conns, int *conncount, int i, intptr_t **freebufs) {
	connection *conn = conns + i;
	close(conn->clientfd);
	close(conn->serverfd);

	free_buffer(freebufs, conn->client_buf.data);
	free_buffer(freebufs, conn->server_buf.data);

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

int main(int argc, char **argv) {
	int ep = epoll_create(20);
	if(ep < 0) {
		fatal_error("epoll_create");
	}
	int serverfd = get_server_socket(argv[1], argv[2]);
	add_epoll_fd(EPOLLIN, ep, serverfd);
	struct epoll_event events[32];
	connection conns[MAXCONNS];
	int conncount = 0;

	intptr_t *freebufs = 0;
	time_t last_print = 0;

	for(;;) {
		int fdcount = epoll_wait(ep, events, arrlen(events), -1);
		for(int i = 0; i < fdcount; i++) {
			struct epoll_event *event = events + i;
			int sockfd = event->data.fd;
			if(event->data.fd == serverfd) {
				if(event->events == EPOLLIN) {
					uint8_t added = add_connection(serverfd, ep, conns, &conncount, &freebufs);
				} else {
					fprintf(stderr, "server event not EPOLLIN but %d\n", event->events);
					exit(1);
				}
			} else {
				if(event->events & EPOLLIN) {
					set_read_ready(sockfd, conns, conncount);
				}
				if(event->events & EPOLLOUT) {
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

				// Refactor: is_buf_full is not necessary it is checked inside read_to_buffer
				while(conn->clientfd >= 0 && conn->clientfd_read_ready) { // && !is_buf_full(&conn->client_buf)) {
					read_to_buffer(&conn->clientfd, &conn->client_buf, &conn->clientfd_read_ready);
				}
				if(conn->client_buf.len >= 10) {
					uint8_t method_match = 
						!strncmp((char *)conn->client_buf.data, "GET ", 4) 		|| 
						!strncmp((char *)conn->client_buf.data, "POST ", 5) 	|| 
						!strncmp((char *)conn->client_buf.data, "DELETE ", 7) 	|| 
						!strncmp((char *)conn->client_buf.data, "OPTIONS ", 8) 	|| 
						!strncmp((char *)conn->client_buf.data, "PUT ", 4)		||
						!strncmp((char *)conn->client_buf.data, "CONNECT ", 8);

					if(method_match) {
						uint8_t found_host = 0;
						char host[256];
						char *port = "80";
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

									for(int i = 0; i < hostlen; i++) {
										if(host[i] == ':') {
											host[i] = 0;
											port = host + i + 1;
											break;
										}
									}
									break;
								} else {
									fatal_error("host length too long");
								}
							} else {
								i += get_next_line(&conn->client_buf, i + 2) + 2;
							}
						}

						//printf("Host: %s\n", host);
						//printf("Port: %s\n", port);

						if(found_host) {
							struct addrinfo hints;
							struct addrinfo *ai;
							memset(&hints, 0, sizeof(struct addrinfo));
							hints.ai_family = AF_INET;
							hints.ai_socktype= SOCK_STREAM;
							hints.ai_flags = AI_PASSIVE; // remove when start using local_addr?
							if(getaddrinfo(host, port, &hints, &ai) == 0) {
								int serverfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
								if(serverfd < 0) {
									fatal_error("socket");
								}

								/*struct sockaddr_in local_addr;
								memset(&local_addr, 0, sizeof(local_addr));
								local_addr.sin_family = AF_INET;
								local_addr.sin_addr.s_addr = inet_addr(argv[2]);
								local_addr.sin_port = htons(0);
								if(bind(serverfd, (struct sockaddr *)&local_addr, sizeof(struct sockaddr)) != 0) {
									printf("errno=%d\n", errno);
									fatal_error("bind");
								}*/

								if(connect(serverfd, ai->ai_addr, ai->ai_addrlen) == 0) {
									conn->serverfd = serverfd;
									fcntl(serverfd, F_SETFL, O_NONBLOCK);
									add_epoll_fd(EPOLLIN | EPOLLOUT | EPOLLET, ep, serverfd);

									if(!strncmp((char *)conn->client_buf.data, "CONNECT ", 8)) {
										conn->client_buf.len = 0;
										char *resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
										int respbytes = strlen(resp);
										memcpy(conn->server_buf.data, resp, respbytes);
										conn->server_buf.len = respbytes;
									}
								} else {
									printf("failed to connect\n");
								}
							} else{
								printf("failed to getaddrinfo\n");
							}

							conn->tunneled = 1;
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
				if(conn->serverfd >= 0) read_to_buffer(&conn->serverfd, &conn->server_buf, &conn->serverfd_read_ready);
				if(conn->clientfd >= 0) write_from_buffer(&conn->clientfd, &conn->server_buf, &conn->clientfd_write_ready);

				keep_going = 
					(conn->serverfd >= 0 && conn->serverfd_write_ready && !is_buf_empty(&conn->client_buf)) ||
					(conn->clientfd >= 0 && conn->clientfd_write_ready && !is_buf_empty(&conn->server_buf)) ||
					(conn->serverfd >= 0 && conn->serverfd_read_ready && !is_buf_full(&conn->server_buf)) ||
					(conn->clientfd >= 0 && conn->clientfd_read_ready && !is_buf_full(&conn->client_buf));

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
				kill_connection(conns, &conncount, i, &freebufs);
			} else {
				i++;
			}
		}

		time_t now = time(0);
		if(now - last_print >= 10) {
			printf("connections=%4d\n", conncount);
			last_print = now;
		}
	}
}
