/*
 * SysDB - src/plugins/timeseries/rrdtool.c
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

#if HAVE_CONFIG_H
#	include "config.h"
#endif /* HAVE_CONFIG_H */

#include "sysdb.h"
#include "core/plugin.h"
#include "utils/error.h"

#include "liboconfig/utils.h"

#include <errno.h>
#include <stdlib.h>
#include <rrd.h>
#ifdef HAVE_RRD_CLIENT_H
#	include <rrd_client.h>
#endif

SDB_PLUGIN_MAGIC;

/* Current versions of RRDtool do not support multiple RRDCacheD client
 * connections. Use this to guard against multiple configured RRDCacheD
 * instances. */
static bool rrdcached_in_use = 0;

/*
 * plugin API
 */

static sdb_timeseries_t *
sdb_rrd_fetch(const char *id, sdb_timeseries_opts_t *opts,
		sdb_object_t *user_data)
{
	sdb_timeseries_t *ts;

	time_t start = (time_t)SDB_TIME_TO_SECS(opts->start);
	time_t end = (time_t)SDB_TIME_TO_SECS(opts->end);
	time_t t;

	unsigned long step = 0;
	unsigned long ds_cnt = 0;
	unsigned long val_cnt = 0;
	char **ds_namv = NULL;
	rrd_value_t *data = NULL, *data_ptr;

	if (user_data) {
#ifdef HAVE_RRD_CLIENT_H
		/* -> use RRDCacheD */
		char *addr = SDB_OBJ_WRAPPER(user_data)->data;

		rrd_clear_error();
		if (! rrdc_is_connected(addr)) {
			if (rrdc_connect(addr)) {
				sdb_log(SDB_LOG_ERR, "timeseries::rrdtool: Failed to "
						"connectd to RRDCacheD at %s: %s",
						addr, rrd_get_error());
				return NULL;
			}
		}

		if (rrdc_flush(id)) {
			sdb_log(SDB_LOG_ERR, "timeseries::rrdtool: Failed to flush "
					"'%s' through RRDCacheD: %s", id, rrd_get_error());
			return NULL;
		}
#else
		sdb_log(SDB_LOG_ERR, "timeseries::rrdtool: Callback called with "
				"RRDCacheD address but your build of SysDB does not support "
				"that");
		return NULL;
#endif
	}

#define FREE_RRD_DATA() \
	do { \
		size_t i; \
		for (i = 0; i < ds_cnt; ++i) \
			rrd_freemem(ds_namv[i]); \
		rrd_freemem(ds_namv); \
		rrd_freemem(data); \
	} while (0)

	/* limit to about 1000 data-points for now
	 * TODO: make this configurable */
	step = (end - start) / 1000;

	if (rrd_fetch_r(id, "AVERAGE", &start, &end, &step,
				&ds_cnt, &ds_namv, &data)) {
		char errbuf[1024];
		sdb_strerror(errno, errbuf, sizeof(errbuf));
		sdb_log(SDB_LOG_ERR, "rrdtool plugin: Failed to fetch data "
				"from %s: %s", id, errbuf);
		return NULL;
	}

	val_cnt = (unsigned long)(end - start) / step;

	ts = sdb_timeseries_create(ds_cnt, (const char * const *)ds_namv, val_cnt);
	if (! ts) {
		char errbuf[1024];
		sdb_strerror(errno, errbuf, sizeof(errbuf));
		sdb_log(SDB_LOG_ERR, "rrdtool plugin: Failed to allocate "
				"time-series object: %s", errbuf);
		FREE_RRD_DATA();
		return NULL;
	}

	ts->start = SECS_TO_SDB_TIME(start + (time_t)step);
	ts->end = SECS_TO_SDB_TIME(end);

	data_ptr = data;
	for (t = start + (time_t)step; t <= end; t += (time_t)step) {
		unsigned long i, j;

		i = (unsigned long)(t - start) / step - 1;

		for (j = 0; j < ds_cnt; ++j) {
			ts->data[j][i].timestamp = SECS_TO_SDB_TIME(t);
			ts->data[j][i].value = *data_ptr;
			++data_ptr;
		}
	}

	FREE_RRD_DATA();
	return ts;
} /* sdb_rrd_fetch */

static int
sdb_rrdcached_shutdown(sdb_object_t __attribute__((unused)) *user_data)
{
#ifdef HAVE_RRD_CLIENT_H
	rrdc_disconnect();
#endif
	return 0;
} /* sdb_rrdcached_shutdown */

static int
sdb_rrd_config_rrdcached(oconfig_item_t *ci)
{
	sdb_object_t *ud;
	char *addr = NULL;

	if (rrdcached_in_use) {
		sdb_log(SDB_LOG_ERR, "timeseries::rrdtool: RRDCacheD does "
				"not support multiple connections");
		return -1;
	}

#ifndef HAVE_RRD_CLIENT_H
	sdb_log(SDB_LOG_ERR, "timeseries::rrdtool: RRDCacheD client "
			"support not available in your SysDB build");
	return -1;
#else
	if (oconfig_get_string(ci, &addr)) {
		sdb_log(SDB_LOG_ERR, "timeseries::unixsock: RRDCacheD requires "
				"a single string argument\n\tUsage <RRDCacheD ADDR>");
		return -1;
	}
	if ((*addr != '/') && strncmp(addr, "unix:", strlen("unix:"))) {
		/* XXX: add (optional) support for rrdc_fetch if available */
		sdb_log(SDB_LOG_ERR, "timeseries::unixsock: RRDCacheD only "
				"supports local (UNIX socket) addresses");
		return -1;
	}

	addr = strdup(addr);
	if (! addr) {
		char errbuf[1024];
		sdb_log(SDB_LOG_ERR, "timeseries::unixsock: Failed to duplicate "
				"string: %s", sdb_strerror(errno, errbuf, sizeof(errbuf)));
		return -1;
	}
	if (ci->children_num)
		sdb_log(SDB_LOG_WARNING, "timeseries::unixsock: RRDCacheD does "
				"not support any child config options");

	ud = sdb_object_create_wrapper("rrdcached-addr", addr, free);
	if (! ud) {
		char errbuf[1024];
		sdb_log(SDB_LOG_ERR, "timeseries::unixsock: Failed to create "
				"user-data object: %s",
				sdb_strerror(errno, errbuf, sizeof(errbuf)));
		free(addr);
		return -1;
	}

	sdb_plugin_register_ts_fetcher("rrdcached", sdb_rrd_fetch, ud);
	sdb_plugin_register_shutdown("rrdcached", sdb_rrdcached_shutdown, NULL);
	sdb_object_deref(ud);
	rrdcached_in_use = 1;
	return 0;
#endif
} /* sdb_rrd_config_rrdcached */

static int
sdb_rrd_config(oconfig_item_t *ci)
{
	int i;

	if (! ci) { /* reconfigure */
		rrdcached_in_use = 0;
		return 0;
	}

	for (i = 0; i < ci->children_num; ++i) {
		oconfig_item_t *child = ci->children + i;

		if (! strcasecmp(child->key, "RRDCacheD"))
			sdb_rrd_config_rrdcached(child);
		else
			sdb_log(SDB_LOG_WARNING, "timeseries::rrdtool: Ignoring "
					"unknown config option '%s'.", child->key);
	}
	return 0;
} /* sdb_rrd_config */

int
sdb_module_init(sdb_plugin_info_t *info)
{
	sdb_plugin_set_info(info, SDB_PLUGIN_INFO_DESC,
			"fetch time-series from RRD files");
	sdb_plugin_set_info(info, SDB_PLUGIN_INFO_COPYRIGHT,
			"Copyright (C) 2014 Sebastian 'tokkee' Harl <sh@tokkee.org>");
	sdb_plugin_set_info(info, SDB_PLUGIN_INFO_LICENSE, "BSD");
	sdb_plugin_set_info(info, SDB_PLUGIN_INFO_VERSION, SDB_VERSION);
	sdb_plugin_set_info(info, SDB_PLUGIN_INFO_PLUGIN_VERSION, SDB_VERSION);

	sdb_plugin_register_ts_fetcher("rrdtool", sdb_rrd_fetch, NULL);
	sdb_plugin_register_config(sdb_rrd_config);
	return 0;
} /* sdb_module_init */

/* vim: set tw=78 sw=4 ts=4 noexpandtab : */

