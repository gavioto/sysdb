/*
 * SysDB - src/include/core/timeseries.h
 * Copyright (C) 2014 Sebastian 'tokkee' Harl <sh@tokkee.org>
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

#ifndef SDB_CORE_TIMESERIES_H
#define SDB_CORE_TIMESERIES_H 1

#include "sysdb.h"
#include "core/time.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A data-point describes a datum at a certain point of time.
 */
typedef struct {
	sdb_time_t timestamp;
	double value;
} sdb_data_point_t;

/*
 * A timeseries describes one or more sequences of data-points. Multiple
 * sequences will have a name each and share the same start and end times and
 * number of data points.
 *
 * Start and end times may diverge slightly from the requested start and end
 * times depending on the resolution available in the backend data-store.
 */
typedef struct {
	sdb_time_t start;
	sdb_time_t end;

	sdb_data_point_t **data;
	size_t data_len;
	char **data_names;
	size_t data_names_len;
} sdb_timeseries_t;

/*
 * Time-series options specify generic parameters to be used when fetching
 * time-series data from a data-store.
 */
typedef struct {
	sdb_time_t start;
	sdb_time_t end;
} sdb_timeseries_opts_t;

/*
 * sdb_timeseries_create:
 * Allocate a time-series object, pre-populating the data_names information
 * and allocating the data field.
 *
 * Returns:
 *  - a newly allocated time-series object on success
 *  - NULL else
 */
sdb_timeseries_t *
sdb_timeseries_create(size_t data_names_len, const char * const *data_names,
		size_t data_len);

/*
 * sdb_timeseries_destroy:
 * Destroy a time-series object, freeing all of its memory.
 */
void
sdb_timeseries_destroy(sdb_timeseries_t *ts);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ! SDB_CORE_TIMESERIES_H */

/* vim: set tw=78 sw=4 ts=4 noexpandtab : */

