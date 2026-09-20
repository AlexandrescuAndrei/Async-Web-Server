// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
	snprintf(conn->send_buffer, BUFSIZ,
		 "HTTP/1.1 200 OK\r\n"
		 "Content-Length: %zu\r\n"
		 "Connection: close\r\n"
		 "\r\n",
		 conn->file_size);
	conn->send_len = strlen(conn->send_buffer);
	conn->send_pos = 0;
	conn->state = STATE_SENDING_HEADER;
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
	const char *header = "HTTP/1.1 404 Not Found\r\n"
					 "Content-Length: 0\r\n"
					 "Connection: close\r\n"
					 "\r\n";
	snprintf(conn->send_buffer, BUFSIZ, "%s", header);
	conn->send_len = strlen(conn->send_buffer);
	conn->send_pos = 0;
	conn->state = STATE_SENDING_404;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	if (strncmp(conn->request_path, "/static/", 8) == 0)
		return RESOURCE_TYPE_STATIC;
	else if (strncmp(conn->request_path, "/dynamic/", 9) == 0)
		return RESOURCE_TYPE_DYNAMIC;
	return RESOURCE_TYPE_NONE;
}


struct connection *connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
	struct connection *conn = calloc(1, sizeof(*conn));

	if (!conn) {
		perror("calloc");
		return NULL;
	}
	conn->sockfd = sockfd;
	conn->state = STATE_INITIAL;
	conn->ctx = ctx;
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	conn->iocb.aio_lio_opcode = IO_CMD_PREAD;
	conn->iocb.aio_fildes = conn->fd;
	conn->iocb.u.c.buf = conn->send_buffer;
	conn->iocb.u.c.nbytes = BUFSIZ;
	conn->iocb.u.c.offset = conn->file_pos;
	conn->piocb[0] = &conn->iocb;
	conn->state = STATE_ASYNC_ONGOING;
	io_set_eventfd(&conn->iocb, conn->eventfd);
	int ret = io_submit(ctx, 1, conn->piocb);

	if (ret < 0) {
		perror("io_submit");
		connection_remove(conn);
		return;
	}
}

void connection_remove(struct connection *conn)
{
	/* TODO: Remove connection handler. */
	if (conn) {
		conn->state = STATE_CONNECTION_CLOSED;
		epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->sockfd, NULL);
		if (conn->fd > 0)
			close(conn->fd);
		tcp_close_connection(conn->sockfd);
	}
}

void handle_new_connection(void)
{
	/* TODO: Handle a new connection request on the server socket. */
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);

	/* TODO: Accept new connection. */
	int clientfd = accept(listenfd, (struct sockaddr *)&client_addr, &client_len);

	if (clientfd < 0) {
		perror("accept");
		return;
	}

	/* TODO: Set socket to be non-blocking. */
	int flags = fcntl(clientfd, F_GETFL, 0);

	fcntl(clientfd, F_SETFL, flags | O_NONBLOCK);

	/* TODO: Instantiate new connection handler. */
	struct connection *conn = connection_create(clientfd);

	if (!conn) {
		close(clientfd);
		return;
	}

	/* TODO: Add socket to epoll. */
	struct epoll_event ev = {
		.events = EPOLLIN | EPOLLET,
		.data.ptr = conn
	};
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, clientfd, &ev) < 0) {
		perror("epoll_ctl");
		connection_remove(conn);
	}

	/* TODO: Initialize HTTP_REQUEST parser. */
}

void receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	ssize_t nread = recv(conn->sockfd, conn->recv_buffer, BUFSIZ, 0);

	if (nread < 0) {
		perror("recv");
		connection_remove(conn);
		return;
	}
	conn->recv_len = nread;
	if (parse_header(conn) == 0)
		conn->state = STATE_REQUEST_RECEIVED;
	else
		conn->state = STATE_SENDING_404;
	handle_input(conn);
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */
	snprintf(conn->filename, BUFSIZ, "%s%s", AWS_DOCUMENT_ROOT, conn->request_path);
	char *p = strchr(conn->filename + 1, '.');

	if (p)
		strcpy(p, ".dat");
	else
		strcat(conn->filename, ".dat");
	conn->fd = open(conn->filename, O_RDONLY);
	dlog(1, "Opening file %s %d\n", conn->filename, conn->fd);
	if (conn->fd < 0)
		return -1;
	struct stat st;

	if (fstat(conn->fd, &st) < 0) {
		close(conn->fd);
		return -1;
	}
	conn->file_size = st.st_size;
	dlog(1, "File size: %zu\n", conn->file_size);
	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
	struct io_event event;
	int ret = io_getevents(ctx, 1, 1, &event, NULL);

	if (ret < 0) {
		conn->send_len = 0;
		return;
	}
	conn->send_len = event.res;
	conn->file_pos += event.res;
}

int parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	http_parser_settings settings_on_path = {
		.on_path = aws_on_path_cb
	};
	http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);
	return conn->have_path ? 0 : -1;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */
	int bytes = 0;

	while (bytes < conn->file_size) {
		int sent = sendfile(conn->sockfd, conn->fd, NULL, BUFSIZ);

		if (sent < 0) {
			perror("sendfile");
			return STATE_CONNECTION_CLOSED;
		}
		bytes += sent;
	}
	dlog(1, "Sent %d bytes\n", bytes);
	return STATE_NO_STATE;
}

int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	ssize_t bytescnt = 0;
	ssize_t totalcnt = 0;

	while (totalcnt < conn->send_len) {
		bytescnt = send(conn->sockfd, conn->send_buffer + totalcnt, conn->send_len - totalcnt, 0);
		if (bytescnt < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			perror("send");
			return -1;
		}
		totalcnt += bytescnt;
	}
	return totalcnt;
}


int connection_send_dynamic(struct connection *conn)
{
	/* TODO: Read data asynchronously.
	 * Returns 0 on success and -1 on error.
	 */
	struct epoll_event ev = {
		.events = EPOLLIN,
		.data.ptr = conn
	};
	conn->eventfd = eventfd(0, EFD_NONBLOCK);
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, conn->eventfd, &ev) < 0) {
		perror("epoll_ctl");
		return -1;
	}
	connection_start_async_io(conn);
	return 0;
}


void handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */

	switch (conn->state) {
	case STATE_INITIAL:
		conn->state = STATE_RECEIVING_DATA;
		receive_data(conn);
		break;
	case STATE_REQUEST_RECEIVED:
		if (connection_open_file(conn) < 0)
			connection_prepare_send_404(conn);
		else
			connection_prepare_send_reply_header(conn);
		handle_output(conn);
		break;
	case STATE_ASYNC_ONGOING:
		connection_complete_async_io(conn);
		connection_send_data(conn);
		if (conn->file_pos < conn->file_size)
			connection_start_async_io(conn);
		else
			connection_remove(conn);
		break;
	case STATE_SENDING_404:
		connection_prepare_send_404(conn);
		handle_output(conn);
		break;
	default:
		printf("Unexpected state\n");
	}
}

void handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or notification of
	 * completion of an asynchronous I/O operation or invalid requests.
	 */

	switch (conn->state) {
	case STATE_SENDING_HEADER:
		if (connection_send_data(conn) == conn->send_len) {
			conn->state = STATE_HEADER_SENT;
			handle_output(conn);
		}
		break;
	case STATE_HEADER_SENT:
		if (connection_get_resource_type(conn) == RESOURCE_TYPE_STATIC) {
			conn->state = connection_send_static(conn);
			connection_remove(conn);
		} else if (connection_get_resource_type(conn) == RESOURCE_TYPE_DYNAMIC) {
			conn->state = STATE_SENDING_DATA;
			connection_send_dynamic(conn);
		} else {
			dlog(1, "Invalid resource type\n");
			connection_prepare_send_404(conn);
			handle_output(conn);
		}
		break;
	case STATE_SENDING_404:
		if (connection_send_data(conn) == conn->send_len) {
			conn->state = STATE_404_SENT;
			connection_remove(conn);
		}
		break;
	case STATE_SENDING_DATA:
		if (connection_send_data(conn) == conn->send_len)
			conn->state = STATE_DATA_SENT;
		break;
	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
	if (event & EPOLLIN)
		handle_input(conn);
	if (event & EPOLLOUT)
		handle_output(conn);
	if (conn->state == STATE_CONNECTION_CLOSED)
		connection_remove(conn);
}

int main(void)
{
	int rc;

	/* TODO: Initialize asynchronous operations. */

	/* TODO: Initialize multiplexing. */

	/* TODO: Create server socket. */

	/* TODO: Add server socket to epoll object*/

	/* Uncomment the following line for debugging. */
	// dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);
	memset(&ctx, 0, sizeof(ctx));
	if (io_setup(128, &ctx) < 0) {
		perror("io_setup");
		exit(EXIT_FAILURE);
	}

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	int opt = 1;

	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in server_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(AWS_LISTEN_PORT)
	};
	if (bind(listenfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		perror("bind");
		exit(EXIT_FAILURE);
	}

	if (listen(listenfd, SOMAXCONN) < 0) {
		perror("listen");
		exit(EXIT_FAILURE);
	}

	epollfd = epoll_create1(0);
	if (epollfd < 0) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	struct epoll_event ev = {
		.events = EPOLLIN,
		.data.fd = listenfd
	};
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev) < 0) {
		perror("epoll_ctl");
		exit(EXIT_FAILURE);
	}
	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* TODO: Wait for events. */

		/* TODO: Switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
		struct epoll_event events[100];
		int n = epoll_wait(epollfd, events, 100, -1);

		if (n < 0) {
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}
		for (int i = 0; i < n; i++) {
			if (events[i].data.fd == listenfd)
				handle_new_connection();
			else
				handle_input(events[i].data.ptr);
		}
	}
	close(epollfd);
	close(listenfd);
	io_destroy(ctx);
	return 0;
}
