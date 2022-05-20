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
	struct addrinfo *ai;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	int result = getaddrinfo("lexmar.org", "80", &hints, &ai);
	printf("%d\n", result);
}
