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

int main() {
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	struct addrinfo *result;

	if(getaddrinfo("www.cubic.com", "443", &hints, &result) != 0) {
		printf("getaddrinfo failed");
		exit(0);
	}

	for(struct addrinfo *cur = result; cur; cur = cur->ai_next) {
		printf("%p %p\n", cur, cur->ai_next);

		int sockfd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
		if(connect(sockfd, cur->ai_addr, cur->ai_addrlen) != 0) {
			perror("connect");
		} else {
			printf("success\n");
			exit(0);
		}

	}

}
