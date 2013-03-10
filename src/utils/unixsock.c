/*
 * SysDB - src/utils/unixsock.c
 * Copyright (C) 2012 Sebastian 'tokkee' Harl <sh@tokkee.org>
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

#include "utils/unixsock.h"
#include "utils/string.h"

#include <assert.h>
#include <errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <strings.h>

#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

/*
 * private data types
 */

struct sdb_unixsock_client {
	char *path;
	FILE *fh;

	int shutdown;
};

#define SDB_SHUT_RD   (1 << SHUT_RD)
#define SDB_SHUT_WR   (1 << SHUT_WR)
#define SDB_SHUT_RDWR (SDB_SHUT_RD | SDB_SHUT_WR)

/*
 * private helper functions
 */

static int
sdb_unixsock_get_column_count(const char *string, const char *delim)
{
	int count = 1;

	assert(string);

	if ((! delim) || (*string == '\0'))
		return 1;

	if ((delim[0] == '\0') || (delim[1] == '\0')) {
		while ((string = strchr(string, (int)delim[0]))) {
			++string;
			++count;
		}
	}
	else {
		while ((string = strpbrk(string, delim))) {
			++string;
			++count;
		}
	}
	return count;
} /* sdb_unixsock_get_column_count */

static int
sdb_unixsock_parse_cell(char *string, int type, sdb_data_t *data)
{
	char *endptr = NULL;

	switch (type) {
		case SDB_TYPE_INTEGER:
			errno = 0;
			data->data.integer = strtoll(string, &endptr, 0);
			break;
		case SDB_TYPE_DECIMAL:
			errno = 0;
			data->data.decimal = strtod(string, &endptr);
			break;
		case SDB_TYPE_STRING:
			data->data.string = string;
			break;
		case SDB_TYPE_DATETIME:
			{
				double datetime = strtod(string, &endptr);
				data->data.datetime = DOUBLE_TO_SDB_TIME(datetime);
			}
			break;
		case SDB_TYPE_BINARY:
			/* we don't support any binary information containing 0-bytes */
			data->data.binary.length = strlen(string);
			data->data.binary.datum = (const unsigned char *)string;
			break;
		default:
			fprintf(stderr, "unixsock: Unexpected type %i while "
					"parsing query result.\n", type);
			return -1;
	}

	if ((type == SDB_TYPE_INTEGER) || (type == SDB_TYPE_DECIMAL)
			|| (type == SDB_TYPE_DATETIME)) {
		if (errno || (string == endptr)) {
			char errbuf[1024];
			fprintf(stderr, "unixsock: Failed to parse string '%s' "
					"as numeric value (type %i): %s\n", string, type,
					sdb_strerror(errno, errbuf, sizeof(errbuf)));
			return -1;
		}
		else if (endptr && (*endptr != '\0'))
			fprintf(stderr, "unixsock: Ignoring garbage after number "
					"while parsing numeric value (type %i): %s.\n",
					type, endptr);
	}

	data->type = type;
	return 0;
} /* sdb_unixsock_parse_cell */

static int
sdb_unixsock_client_process_one_line(sdb_unixsock_client_t *client,
		char *line, sdb_unixsock_client_data_cb callback,
		sdb_object_t *user_data, const char *delim,
		int column_count, int *types)
{
	sdb_data_t data[column_count];
	char *orig_line = line;

	int i;

	assert(column_count > 0);

	for (i = 0; i < column_count; ++i) {
		char *next;

		if (! line) { /* this must no happen */
			fprintf(stderr, "unixsock: Unexpected EOL while parsing line "
					"(expected %i columns delimited by '%s'; got %i): %s\n",
					column_count, delim, /* last line number */ i, orig_line);
			return -1;
		}

		if ((delim[0] == '\0') || (delim[1] == '\0'))
			next = strchr(line, (int)delim[0]);
		else
			next = strpbrk(line, delim);

		if (next) {
			*next = '\0';
			++next;
		}

		if (sdb_unixsock_parse_cell(line,
					types ? types[i] : SDB_TYPE_STRING, &data[i]))
			return -1;

		line = next;
	}

	if (callback(client, (size_t)column_count, data, user_data))
		return -1;
	return 0;
} /* sdb_unixsock_client_process_one_line */

/*
 * public API
 */

sdb_unixsock_client_t *
sdb_unixsock_client_create(const char *path)
{
	sdb_unixsock_client_t *client;

	if (! path)
		return NULL;

	client = malloc(sizeof(*client));
	if (! client)
		return NULL;
	memset(client, 0, sizeof(*client));
	client->fh = NULL;

	client->path = strdup(path);
	if (! client->path) {
		sdb_unixsock_client_destroy(client);
		return NULL;
	}

	client->shutdown = 0;
	return client;
} /* sdb_unixsock_client_create */

int
sdb_unixsock_client_connect(sdb_unixsock_client_t *client)
{
	struct sockaddr_un sa;
	int fd;

	if ((! client) || (! client->path))
		return -1;

	memset(&sa, 0, sizeof(sa));

	if (client->fh)
		fclose(client->fh);

	fd = socket(AF_UNIX, SOCK_STREAM, /* protocol = */ 0);
	if (fd < 0) {
		char errbuf[1024];
		fprintf(stderr, "unixsock: Failed to open socket: %s\n",
				sdb_strerror(errno, errbuf, sizeof(errbuf)));
		return -1;
	}

	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, client->path, sizeof(sa.sun_path));
	sa.sun_path[sizeof(sa.sun_path) - 1] = '\0';

	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		char errbuf[1024];
		fprintf(stderr, "unixsock: Failed to connect to %s: %s\n",
				sa.sun_path, sdb_strerror(errno, errbuf, sizeof(errbuf)));
		close(fd);
		return -1;
	}

	client->fh = fdopen(fd, "r+");
	if (! client->fh) {
		char errbuf[1024];
		fprintf(stderr, "unixsock: Failed to open I/O stream for %s: %s\n",
				sa.sun_path, sdb_strerror(errno, errbuf, sizeof(errbuf)));
		close(fd);
		return -1;
	}

	client->shutdown = 0;
	return 0;
} /* sdb_unixsock_client_connect */

int
sdb_unixsock_client_send(sdb_unixsock_client_t *client,
		const char *msg)
{
	int status;

	if ((! client) || (! client->fh))
		return -1;

	if (client->shutdown & SDB_SHUT_WR) /* reconnect */
		sdb_unixsock_client_connect(client);

	status = fprintf(client->fh, "%s\r\n", msg);
	if (status < 0) {
		char errbuf[1024];
		fprintf(stderr, "unixsock: Failed to write to socket (%s): %s\n",
				client->path, sdb_strerror(errno, errbuf, sizeof(errbuf)));
		return status;
	}
	return status;
} /* sdb_unixsock_client_send */

char *
sdb_unixsock_client_recv(sdb_unixsock_client_t *client,
		char *buffer, size_t buflen)
{
	if ((! client) || (! client->fh) || (! buffer))
		return NULL;

	if (client->shutdown & SDB_SHUT_RD) /* reconnect */
		sdb_unixsock_client_connect(client);

	buffer = fgets(buffer, (int)buflen - 1, client->fh);
	if (! buffer) {
		if (! feof(client->fh)) {
			char errbuf[1024];
			fprintf(stderr, "unixsock: Failed to read from socket (%s): %s\n",
					client->path, sdb_strerror(errno, errbuf, sizeof(errbuf)));
		}
		return buffer;
	}
	buffer[buflen - 1] = '\0';

	buflen = strlen(buffer);
	while ((buffer[buflen - 1] == '\n') || (buffer[buflen - 1] == '\r')) {
		buffer[buflen - 1] = '\0';
		--buflen;
	}
	return buffer;
} /* sdb_unixsock_client_recv */

int
sdb_unixsock_client_process_lines(sdb_unixsock_client_t *client,
		sdb_unixsock_client_data_cb callback, sdb_object_t *user_data,
		long int max_lines, const char *delim, int n_cols, ...)
{
	int *types = NULL;
	int success = 0;

	if ((! client) || (! client->fh) || (! callback))
		return -1;

	if (n_cols > 0) {
		va_list ap;
		int i;

		types = calloc((size_t)n_cols, sizeof(*types));
		if (! types)
			return -1;

		va_start(ap, n_cols);

		for (i = 0; i < n_cols; ++i) {
			types[i] = va_arg(ap, int);

			if ((types[i] < 1) || (types[i] > SDB_TYPE_BINARY)) {
				fprintf(stderr, "unixsock: Unknown column type %i while "
						"processing response from the UNIX socket @ %s.\n",
						types[i], client->path);
				va_end(ap);
				free(types);
				return -1;
			}
		}

		va_end(ap);
	}

	while (42) {
		char  buffer[1024];
		char *line;

		int column_count;

		if (! max_lines)
			break;

		if (max_lines > 0)
			--max_lines;

		sdb_unixsock_client_clearerr(client);
		line = sdb_unixsock_client_recv(client, buffer, sizeof(buffer));

		if (! line)
			break;

		column_count = sdb_unixsock_get_column_count(line, delim);

		if ((n_cols >= 0) && (n_cols != column_count)) {
			fprintf(stderr, "unixsock: number of columns (%i) does not "
					"match the number of requested columns (%i) while "
					"processing response from the UNIX socket @ %s: %s\n",
					column_count, n_cols, client->path, line);
			continue;
		}

		if (column_count <= 0) /* no data */
			continue;

		if (! sdb_unixsock_client_process_one_line(client, line, callback,
					user_data, delim, column_count, types))
			++success;
	}

	free(types);

	if ((max_lines > 0)
			|| ((max_lines < 0) && (! sdb_unixsock_client_eof(client)))
			|| sdb_unixsock_client_error(client)) {
		char errbuf[1024];
		fprintf(stderr, "unixsock: Unexpected end of data while reading "
				"from socket (%s): %s\n", client->path,
				sdb_strerror(errno, errbuf, sizeof(errbuf)));
		return -1;
	}
	if (! success)
		return -1;
	return 0;
} /* sdb_unixsock_client_process_lines */

int
sdb_unixsock_client_shutdown(sdb_unixsock_client_t *client, int how)
{
	int status;

	if (! client) {
		errno = ENOTSOCK;
		return -1;
	}

	fflush(client->fh);
	status = shutdown(fileno(client->fh), how);

	if (! status) {
		if (how == SHUT_RDWR)
			client->shutdown |= SDB_SHUT_RDWR;
		else
			client->shutdown |= 1 << how;
	}
	return status;
} /* sdb_unixsock_client_shutdown */

void
sdb_unixsock_client_clearerr(sdb_unixsock_client_t *client)
{
	if ((! client) || (! client->fh))
		return;
	clearerr(client->fh);
} /* sdb_unixsock_client_clearerr */

int
sdb_unixsock_client_eof(sdb_unixsock_client_t *client)
{
	if ((! client) || (! client->fh)) {
		errno = EBADF;
		return -1;
	}
	return feof(client->fh);
} /* sdb_unixsock_client_eof */

int
sdb_unixsock_client_error(sdb_unixsock_client_t *client)
{
	if ((! client) || (! client->fh)) {
		errno = EBADF;
		return -1;
	}
	return ferror(client->fh);
} /* sdb_unixsock_client_error */

void
sdb_unixsock_client_destroy(sdb_unixsock_client_t *client)
{
	if (! client)
		return;

	if (client->path)
		free(client->path);
	client->path = NULL;

	if (client->fh)
		fclose(client->fh);
	client->fh = NULL;

	free(client);
} /* sdb_unixsock_client_destroy */

const char *
sdb_unixsock_client_path(sdb_unixsock_client_t *client)
{
	if (! client)
		return NULL;
	return client->path;
} /* sdb_unixsock_client_path */

/* vim: set tw=78 sw=4 ts=4 noexpandtab : */
