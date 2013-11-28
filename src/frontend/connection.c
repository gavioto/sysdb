/*
 * SysDB - src/frontend/connection.c
 * Copyright (C) 2013 Sebastian 'tokkee' Harl <sh@tokkee.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "sysdb.h"
#include "core/error.h"
#include "core/object.h"
#include "frontend/connection-private.h"
#include "utils/strbuf.h"

#include <assert.h>
#include <errno.h>

#include <arpa/inet.h>
#include <fcntl.h>

#include <string.h>

/*
 * private data types
 */

/* name of connection objects */
#define CONN_FD_PREFIX "conn#"
#define CONN_FD_PLACEHOLDER "XXXXXXX"

static int
connection_init(sdb_object_t *obj, va_list ap)
{
	sdb_conn_t *conn;
	int sock_fd;
	int sock_fl;

	assert(obj);
	conn = CONN(obj);

	sock_fd = va_arg(ap, int);

	conn->buf = sdb_strbuf_create(/* size = */ 128);
	if (! conn->buf) {
		sdb_log(SDB_LOG_ERR, "frontend: Failed to allocate a read buffer "
				"for a new connection");
		return -1;
	}

	conn->client_addr_len = sizeof(conn->client_addr);
	conn->fd = accept(sock_fd, (struct sockaddr *)&conn->client_addr,
			&conn->client_addr_len);

	if (conn->fd < 0) {
		char buf[1024];
		sdb_log(SDB_LOG_ERR, "frontend: Failed to accept remote "
				"connection: %s", sdb_strerror(errno,
					buf, sizeof(buf)));
		return -1;
	}

	if (conn->client_addr.ss_family != AF_UNIX) {
		sdb_log(SDB_LOG_ERR, "frontend: Accepted connection using "
				"unexpected family type %d", conn->client_addr.ss_family);
		return -1;
	}

	sock_fl = fcntl(conn->fd, F_GETFL);
	if (fcntl(conn->fd, F_SETFL, sock_fl | O_NONBLOCK)) {
		char buf[1024];
		sdb_log(SDB_LOG_ERR, "frontend: Failed to switch connection conn#%i "
				"to non-blocking mode: %s", conn->fd,
				sdb_strerror(errno, buf, sizeof(buf)));
		return -1;
	}

	sdb_log(SDB_LOG_DEBUG, "frontend: Accepted connection on fd=%i",
			conn->fd);

	conn->cmd = CONNECTION_IDLE;
	conn->cmd_len = 0;

	/* update the object name */
	snprintf(obj->name + strlen(CONN_FD_PREFIX),
			strlen(CONN_FD_PLACEHOLDER), "%i", conn->fd);
	return 0;
} /* connection_init */

static void
connection_destroy(sdb_object_t *obj)
{
	sdb_conn_t *conn;
	size_t len;

	assert(obj);
	conn = CONN(obj);

	if (conn->buf) {
		len = sdb_strbuf_len(conn->buf);
		if (len)
			sdb_log(SDB_LOG_INFO, "frontend: Discarding incomplete command "
					"(%zu byte%s left in buffer)", len, len == 1 ? "" : "s");
	}

	sdb_log(SDB_LOG_DEBUG, "frontend: Closing connection on fd=%i",
			conn->fd);
	close(conn->fd);
	conn->fd = -1;

	sdb_strbuf_destroy(conn->buf);
	conn->buf = NULL;
} /* connection_destroy */

static sdb_type_t connection_type = {
	/* size = */ sizeof(sdb_conn_t),
	/* init = */ connection_init,
	/* destroy = */ connection_destroy,
};

/*
 * connection handler functions
 */

static uint32_t
connection_get_int32(sdb_conn_t *conn, size_t offset)
{
	const char *data;
	uint32_t n;

	assert(conn && (sdb_strbuf_len(conn->buf) >= offset + sizeof(uint32_t)));

	data = sdb_strbuf_string(conn->buf);
	memcpy(&n, data + offset, sizeof(n));
	n = ntohl(n);
	return n;
} /* connection_get_int32 */

static int
command_handle(sdb_conn_t *conn)
{
	int status = -1;

	assert(conn && (conn->cmd != CONNECTION_IDLE));

	sdb_log(SDB_LOG_DEBUG, "frontend: Handling command %u (len: %u)",
			conn->cmd, conn->cmd_len);

	switch (conn->cmd) {
		case CONNECTION_PING:
			status = sdb_connection_ping(conn);
			break;
		case CONNECTION_STARTUP:
			status = sdb_session_start(conn);
			break;

		case CONNECTION_LIST:
			status = sdb_fe_list(conn);
			break;

		default:
		{
			char errbuf[1024];
			sdb_log(SDB_LOG_WARNING, "frontend: Ignoring invalid command");
			snprintf(errbuf, sizeof(errbuf), "Invalid command %#x", conn->cmd);
			sdb_connection_send(conn, CONNECTION_ERROR,
					(uint32_t)(strlen(errbuf) + 1), errbuf);
			status = -1;
			break;
		}
	}

	/* remove the command from the buffer */
	if (conn->cmd_len)
		sdb_strbuf_skip(conn->buf, conn->cmd_len);
	conn->cmd = CONNECTION_IDLE;
	conn->cmd_len = 0;
	return status;
} /* command_handle */

/* initialize the connection state information */
static int
command_init(sdb_conn_t *conn)
{
	size_t len;

	assert(conn && (conn->cmd == CONNECTION_IDLE) && (! conn->cmd_len));

	conn->cmd = connection_get_int32(conn, 0);
	conn->cmd_len = connection_get_int32(conn, sizeof(uint32_t));

	len = 2 * sizeof(uint32_t);
	if (conn->cmd == CONNECTION_IDLE)
		len += conn->cmd_len;
	sdb_strbuf_skip(conn->buf, len);
	return 0;
} /* command_init */

/* returns negative value on error, 0 on EOF, number of octets else */
static ssize_t
connection_read(sdb_conn_t *conn)
{
	ssize_t n = 0;

	while (42) {
		ssize_t status;

		errno = 0;
		status = sdb_strbuf_read(conn->buf, conn->fd, 1024);
		if (status < 0) {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
				break;
			return (int)status;
		}
		else if (! status) /* EOF */
			break;

		n += status;
	}

	return n;
} /* connection_read */

/*
 * public API
 */

sdb_conn_t *
sdb_connection_accept(int fd)
{
	if (fd < 0)
		return NULL;

	/* the placeholder will be replaced with the accepted file
	 * descriptor when initializing the object */
	return CONN(sdb_object_create(CONN_FD_PREFIX CONN_FD_PLACEHOLDER,
				connection_type, fd));
} /* sdb_connection_create */

void
sdb_connection_close(sdb_conn_t *conn)
{
	sdb_object_deref(SDB_OBJ(conn));
} /* sdb_connection_close */

ssize_t
sdb_connection_read(sdb_conn_t *conn)
{
	ssize_t n = 0;

	while (42) {
		ssize_t status = connection_read(conn);

		if ((conn->cmd == CONNECTION_IDLE) && (! conn->cmd_len)
				&& (sdb_strbuf_len(conn->buf) >= 2 * sizeof(int32_t)))
			command_init(conn);
		if ((conn->cmd != CONNECTION_IDLE)
				&& (sdb_strbuf_len(conn->buf) >= conn->cmd_len))
			command_handle(conn);

		if (status <= 0)
			break;

		n += status;
	}
	return n;
} /* sdb_connection_read */

ssize_t
sdb_connection_send(sdb_conn_t *conn, uint32_t code,
		uint32_t msg_len, const char *msg)
{
	size_t len = 2 * sizeof(uint32_t) + msg_len;
	char buffer[len];
	char *buf;

	uint32_t tmp;

	if ((! conn) || (conn->fd < 0))
		return -1;

	tmp = htonl(code);
	memcpy(buffer, &tmp, sizeof(uint32_t));

	tmp = htonl(msg_len);
	memcpy(buffer + sizeof(uint32_t), &tmp, sizeof(uint32_t));

	if (msg_len)
		memcpy(buffer + 2 * sizeof(uint32_t), msg, msg_len);

	buf = buffer;
	while (len > 0) {
		ssize_t status;

		/* XXX: use select() */

		errno = 0;
		status = write(conn->fd, buf, len);
		if (status < 0) {
			char errbuf[1024];

			if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
				continue;
			if (errno == EINTR)
				continue;

			sdb_log(SDB_LOG_ERR, "frontend: Failed to send msg "
					"(code: %u, len: %u) to client: %s", code, msg_len,
					sdb_strerror(errno, errbuf, sizeof(errbuf)));
			return status;
		}

		len -= (size_t)status;
		buf += status;
	}
	return (ssize_t)len;
} /* sdb_connection_send */

int
sdb_connection_ping(sdb_conn_t *conn)
{
	if ((! conn) || (conn->cmd != CONNECTION_PING))
		return -1;

	/* we're alive */
	sdb_connection_send(conn, CONNECTION_OK, 0, NULL);
	return 0;
} /* sdb_connection_ping */

/* vim: set tw=78 sw=4 ts=4 noexpandtab : */

