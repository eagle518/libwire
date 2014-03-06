#include "xcoro.h"
#include "xcoro_fd.h"
#include "xcoro_task_pool.h"
#include "xcoro_stack.h"
#include "macros.h"
#include "http_parser.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>

static xcoro_t xcoro_main;
static xcoro_task_t task_accept;
static xcoro_task_pool_t web_pool;

struct web_data {
	int fd;
	xcoro_fd_state_t fd_state;
};

static void set_nonblock(int fd)
{
	int ret = fcntl(fd, F_GETFL);
	if (ret < 0)
		return;

	fcntl(fd, F_SETFL, ret | O_NONBLOCK);
}

static void set_reuse(int fd)
{
	int so_reuseaddr = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr));
}

static int socket_setup(unsigned short port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 1) {
		perror("Failed to create socket");
		return -1;
	}

	set_nonblock(fd);
	set_reuse(fd);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	int ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret < 0) {
		perror("Failed to bind to socket");
		close(fd);
		return -1;
	}

	ret = listen(fd, 100);
	if (ret < 0) {
		perror("failed to listen to port");
		close(fd);
		return -1;
	}

	return fd;
}

static int on_message_begin(http_parser *parser)
{
	printf("Message begin\n");
	return 0;
}

static int on_headers_complete(http_parser *parser)
{
	printf("Headers complete: HTTP/%d.%d %s\n", parser->http_major, parser->http_minor, http_method_str(parser->method));
	return 0;
}

static int buf_write(xcoro_fd_state_t *fd_state, const char *buf, int len)
{
	int sent = 0;
	do {
		int ret = write(fd_state->fd, buf + sent, strlen(buf) - sent);
		if (ret == 0)
			return -1;
		else if (ret > 0) {
			sent += ret;
			if (sent == strlen(buf))
				return 0;
		} else {
			// Error
			if (errno != EINTR && errno != EAGAIN)
				return -1;
		}

		xcoro_fd_mode_write(fd_state);
		xcoro_fd_wait(fd_state);
	} while (1);
}

static int on_message_complete(http_parser *parser)
{
	printf("message complete\n");
	struct web_data *d = parser->data;
	char buf[512];
	char data[512] = "Test\r\n";
	snprintf(buf, sizeof(buf), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n%s\r\n",
			(int)strlen(data),
			!http_should_keep_alive(parser) ? "Connection: close\r\n" : "");
	buf_write(&d->fd_state, buf, strlen(buf));
	buf_write(&d->fd_state, data, strlen(data));

	return -1;
}

static int on_url(http_parser *parser, const char *at, size_t length)
{
	printf("URL: %.*s\n", (int)length, at);
	return 0;
}

static int on_status(http_parser *parser, const char *at, size_t length)
{
	printf("STATUS: %.*s\n", (int)length, at);
	return 0;
}

static int on_header_field(http_parser *parser, const char *at, size_t length)
{
	printf("HEADER FIELD: %.*s\n", (int)length, at);
	return 0;
}

static int on_header_value(http_parser *parser, const char *at, size_t length)
{
	printf("HEADER VALUE: %.*s\n", (int)length, at);
	return 0;
}

static int on_body(http_parser *parser, const char *at, size_t length)
{
	printf("BODY: %.*s\n", (int)length, at);
	return 0;
}

static const struct http_parser_settings parser_settings = {
	.on_message_begin = on_message_begin,
	.on_headers_complete = on_headers_complete,
	.on_message_complete = on_message_complete,

	.on_url = on_url,
	.on_status = on_status,
	.on_header_field = on_header_field,
	.on_header_value = on_header_value,
	.on_body = on_body,
};

void task_web_run(void *arg)
{
	struct web_data d = {
		.fd = (long int)arg,
	};
	http_parser parser;

	xcoro_fd_mode_init(&d.fd_state, d.fd);
	xcoro_fd_mode_read(&d.fd_state);

	set_nonblock(d.fd);

	http_parser_init(&parser, HTTP_REQUEST);
	parser.data = &d;

	char buf[4096];
	do {
		buf[0] = 0;
		int received = read(d.fd, buf, sizeof(buf));
		printf("Received: %d %d\n", received, errno);
		if (received == 0) {
			/* Fall-through, tell parser about EOF */
			printf("Received EOF\n");
		} else if (received < 0) {
			printf("Error\n");
			if (errno == EINTR || errno == EAGAIN) {
				printf("Waiting\n");
				/* Nothing received yet, wait for it */
				xcoro_fd_wait(&d.fd_state);
				printf("Done waiting\n");
				continue;
			} else {
				printf("breaking out\n");
				break;
			}
		}

		printf("Processing %d\n", (int)received);
		size_t processed = http_parser_execute(&parser, &parser_settings, buf, received);
		if (parser.upgrade) {
			/* Upgrade not supported yet */
			printf("Upgrade no supported, bailing out\n");
			break;
		} else if (received == 0) {
			// At EOF, exit now
			printf("Received EOF\n");
			break;
		} else if (processed != received) {
			// Error in parsing
			printf("Not everything was parsed, error is likely, bailing out.\n");
			break;
		}
	} while (1);

	xcoro_fd_mode_none(&d.fd_state);
	close(d.fd);
}

void task_accept_run(void *arg)
{
	int fd = socket_setup(9090);
	if (fd < 0)
		return;

	xcoro_fd_state_t fd_state;
	xcoro_fd_mode_init(&fd_state, fd);
	xcoro_fd_mode_read(&fd_state);

	while (1) {
		xcoro_fd_wait(&fd_state);
		int new_fd = accept(fd, NULL, NULL);
		if (new_fd >= 0) {
			printf("New connection: %d\n", new_fd);
			char name[32];
			snprintf(name, sizeof(name), "web %d", new_fd);
			xcoro_task_t *task = xcoro_task_pool_alloc(&web_pool, name, task_web_run, (void*)(long int)new_fd);
			if (!task) {
				printf("Web server is busy, sorry\n");
				close(new_fd);
			}
		} else {
			if (errno != EINTR && errno != EAGAIN) {
				perror("Error accepting from listening socket");
				break;
			}
		}
	}
}

int main()
{
	xcoro_init(&xcoro_main);
	xcoro_fd_init();
	xcoro_task_pool_init(&web_pool, NULL, 16, 16*1024);
	xcoro_task_init(&task_accept, "accept", task_accept_run, NULL, xcoro_stack_alloc(4096), 4096);
	xcoro_run();
	return 0;
}