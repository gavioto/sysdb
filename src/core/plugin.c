/*
 * SysDB - src/core/plugin.c
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

#if HAVE_CONFIG_H
#	include "config.h"
#endif /* HAVE_CONFIG_H */

#include "sysdb.h"
#include "core/plugin.h"
#include "core/time.h"
#include "utils/error.h"
#include "utils/llist.h"
#include "utils/strbuf.h"

#include <assert.h>

#include <errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <ltdl.h>

#include <pthread.h>

/* helper to access info attributes */
#define INFO_GET(i, attr) \
	((i)->attr ? (i)->attr : #attr" not set")

/*
 * private data types
 */

typedef struct {
	sdb_object_t super;
	sdb_plugin_ctx_t public;

	sdb_plugin_info_t info;
	lt_dlhandle handle;

	/* The usage count differs from the object's ref count
	 * in that it provides higher level information about how
	 * the plugin is in use. */
	size_t use_cnt;
} ctx_t;
#define CTX_INIT { SDB_OBJECT_INIT, \
	SDB_PLUGIN_CTX_INIT, SDB_PLUGIN_INFO_INIT, NULL, 0 }

#define CTX(obj) ((ctx_t *)(obj))

typedef struct {
	sdb_object_t super;
	void *cb_callback;
	sdb_object_t *cb_user_data;
	ctx_t *cb_ctx;
} sdb_plugin_cb_t;
#define SDB_PLUGIN_CB_INIT { SDB_OBJECT_INIT, \
	/* callback = */ NULL, /* user_data = */ NULL, \
	SDB_PLUGIN_CTX_INIT }
#define SDB_PLUGIN_CB(obj) ((sdb_plugin_cb_t *)(obj))
#define SDB_CONST_PLUGIN_CB(obj) ((const sdb_plugin_cb_t *)(obj))

typedef struct {
	sdb_plugin_cb_t super;
#define ccb_callback super.cb_callback
#define ccb_user_data super.cb_user_data
#define ccb_ctx super.cb_ctx
	sdb_time_t ccb_interval;
	sdb_time_t ccb_next_update;
} sdb_plugin_collector_cb_t;
#define SDB_PLUGIN_CCB(obj) ((sdb_plugin_collector_cb_t *)(obj))
#define SDB_CONST_PLUGIN_CCB(obj) ((const sdb_plugin_collector_cb_t *)(obj))

typedef struct {
	sdb_plugin_cb_t super; /* cb_callback will always be NULL */
#define w_user_data super.cb_user_data
#define w_ctx super.cb_ctx
	sdb_store_writer_t impl;
} sdb_plugin_writer_t;
#define SDB_PLUGIN_WRITER(obj) ((sdb_plugin_writer_t *)(obj))

/*
 * private variables
 */

static sdb_plugin_ctx_t  plugin_default_ctx  = SDB_PLUGIN_CTX_INIT;
static sdb_plugin_info_t plugin_default_info = SDB_PLUGIN_INFO_INIT;

static pthread_key_t     plugin_ctx_key;
static bool              plugin_ctx_key_initialized = 0;

/* a list of the plugin contexts of all registered plugins */
static sdb_llist_t      *all_plugins = NULL;

static sdb_llist_t      *config_list = NULL;
static sdb_llist_t      *init_list = NULL;
static sdb_llist_t      *collector_list = NULL;
static sdb_llist_t      *cname_list = NULL;
static sdb_llist_t      *shutdown_list = NULL;
static sdb_llist_t      *log_list = NULL;
static sdb_llist_t      *ts_fetcher_list = NULL;
static sdb_llist_t      *writer_list = NULL;

static struct {
	const char   *type;
	sdb_llist_t **list;
} all_lists[] = {
	{ "config",             &config_list },
	{ "init",               &init_list },
	{ "collector",          &collector_list },
	{ "cname",              &cname_list },
	{ "shutdown",           &shutdown_list },
	{ "log",                &log_list },
	{ "timeseries fetcher", &ts_fetcher_list },
	{ "store writer",       &writer_list },
};

/*
 * private helper functions
 */

static void
plugin_info_clear(sdb_plugin_info_t *info)
{
	sdb_plugin_info_t empty_info = SDB_PLUGIN_INFO_INIT;
	if (! info)
		return;

	if (info->plugin_name)
		free(info->plugin_name);
	if (info->filename)
		free(info->filename);

	if (info->description)
		free(info->description);
	if (info->copyright)
		free(info->copyright);
	if (info->license)
		free(info->license);

	*info = empty_info;
} /* plugin_info_clear */

static void
ctx_key_init(void)
{
	if (plugin_ctx_key_initialized)
		return;

	pthread_key_create(&plugin_ctx_key, /* destructor */ NULL);
	plugin_ctx_key_initialized = 1;
} /* ctx_key_init */

static int
plugin_cmp_next_update(const sdb_object_t *a, const sdb_object_t *b)
{
	const sdb_plugin_collector_cb_t *ccb1
		= (const sdb_plugin_collector_cb_t *)a;
	const sdb_plugin_collector_cb_t *ccb2
		= (const sdb_plugin_collector_cb_t *)b;

	assert(ccb1 && ccb2);

	return (ccb1->ccb_next_update > ccb2->ccb_next_update)
		? 1 : (ccb1->ccb_next_update < ccb2->ccb_next_update)
		? -1 : 0;
} /* plugin_cmp_next_update */

static int
plugin_lookup_by_name(const sdb_object_t *obj, const void *id)
{
	const sdb_plugin_cb_t *cb = SDB_CONST_PLUGIN_CB(obj);
	const char *name = id;

	assert(cb && id);

	/* when a plugin was registered from outside a plugin (e.g. the core),
	 * we don't have a plugin context */
	if (! cb->cb_ctx)
		return 1;

	if (!strcasecmp(cb->cb_ctx->info.plugin_name, name))
		return 0;
	return 1;
} /* plugin_lookup_by_name */

/* since this function is called from sdb_plugin_reconfigure_finish()
 * when iterating through all_plugins, we may not do any additional
 * modifications to all_plugins except for the optional removal */
static void
plugin_unregister_by_name(const char *plugin_name)
{
	sdb_object_t *obj;
	size_t i;

	for (i = 0; i < SDB_STATIC_ARRAY_LEN(all_lists); ++i) {
		const char  *type =  all_lists[i].type;
		sdb_llist_t *list = *all_lists[i].list;

		while (1) {
			sdb_plugin_cb_t *cb;

			cb = SDB_PLUGIN_CB(sdb_llist_remove(list,
						plugin_lookup_by_name, plugin_name));
			if (! cb)
				break;

			assert(cb->cb_ctx);

			sdb_log(SDB_LOG_INFO, "core: Unregistering "
					"%s callback '%s' (module %s)", type, cb->super.name,
					cb->cb_ctx->info.plugin_name);
			sdb_object_deref(SDB_OBJ(cb));
		}
	}

	obj = sdb_llist_search_by_name(all_plugins, plugin_name);
	/* when called from sdb_plugin_reconfigure_finish, the object has already
	 * been removed from the list */
	if (obj && (obj->ref_cnt <= 1)) {
		sdb_llist_remove_by_name(all_plugins, plugin_name);
		sdb_object_deref(obj);
	}
	/* else: other callbacks still reference it */
} /* plugin_unregister_by_name */

/*
 * private types
 */

static int
ctx_init(sdb_object_t *obj, va_list __attribute__((unused)) ap)
{
	ctx_t *ctx = CTX(obj);

	assert(ctx);

	ctx->public = plugin_default_ctx;
	ctx->info = plugin_default_info;
	ctx->handle = NULL;
	ctx->use_cnt = 1;
	return 0;
} /* ctx_init */

static void
ctx_destroy(sdb_object_t *obj)
{
	ctx_t *ctx = CTX(obj);

	if (ctx->handle) {
		const char *err;

		sdb_log(SDB_LOG_INFO, "core: Unloading module %s",
				ctx->info.plugin_name);

		lt_dlerror();
		lt_dlclose(ctx->handle);
		if ((err = lt_dlerror()))
			sdb_log(SDB_LOG_WARNING, "core: Failed to unload module %s: %s",
					ctx->info.plugin_name, err);
	}

	plugin_info_clear(&ctx->info);
} /* ctx_destroy */

static sdb_type_t ctx_type = {
	sizeof(ctx_t),

	ctx_init,
	ctx_destroy
};

static ctx_t *
ctx_get(void)
{
	if (! plugin_ctx_key_initialized)
		ctx_key_init();
	return pthread_getspecific(plugin_ctx_key);
} /* ctx_get */

static ctx_t *
ctx_set(ctx_t *new)
{
	ctx_t *old;

	if (! plugin_ctx_key_initialized)
		ctx_key_init();

	old = pthread_getspecific(plugin_ctx_key);
	if (old)
		sdb_object_deref(SDB_OBJ(old));
	if (new)
		sdb_object_ref(SDB_OBJ(new));
	pthread_setspecific(plugin_ctx_key, new);
	return old;
} /* ctx_set */

static ctx_t *
ctx_create(const char *name)
{
	ctx_t *ctx;

	ctx = CTX(sdb_object_create(name, ctx_type));
	if (! ctx)
		return NULL;

	if (! plugin_ctx_key_initialized)
		ctx_key_init();
	ctx_set(ctx);
	return ctx;
} /* ctx_create */

static int
plugin_cb_init(sdb_object_t *obj, va_list ap)
{
	sdb_llist_t **list = va_arg(ap, sdb_llist_t **);
	const char   *type = va_arg(ap, const char *);
	void     *callback = va_arg(ap, void *);
	sdb_object_t   *ud = va_arg(ap, sdb_object_t *);

	assert(list);
	assert(type);
	assert(obj);

	if (sdb_llist_search_by_name(*list, obj->name)) {
		sdb_log(SDB_LOG_WARNING, "core: %s callback '%s' "
				"has already been registered. Ignoring newly "
				"registered version.", type, obj->name);
		return -1;
	}

	/* cb_ctx may be NULL if the plugin was not registered by a plugin */

	SDB_PLUGIN_CB(obj)->cb_callback = callback;
	SDB_PLUGIN_CB(obj)->cb_ctx      = ctx_get();
	sdb_object_ref(SDB_OBJ(SDB_PLUGIN_CB(obj)->cb_ctx));

	sdb_object_ref(ud);
	SDB_PLUGIN_CB(obj)->cb_user_data = ud;
	return 0;
} /* plugin_cb_init */

static void
plugin_cb_destroy(sdb_object_t *obj)
{
	assert(obj);
	sdb_object_deref(SDB_PLUGIN_CB(obj)->cb_user_data);
	sdb_object_deref(SDB_OBJ(SDB_PLUGIN_CB(obj)->cb_ctx));
} /* plugin_cb_destroy */

static sdb_type_t sdb_plugin_cb_type = {
	sizeof(sdb_plugin_cb_t),

	plugin_cb_init,
	plugin_cb_destroy
};

static sdb_type_t sdb_plugin_collector_cb_type = {
	sizeof(sdb_plugin_collector_cb_t),

	plugin_cb_init,
	plugin_cb_destroy
};

static int
plugin_writer_init(sdb_object_t *obj, va_list ap)
{
	sdb_store_writer_t *impl = va_arg(ap, sdb_store_writer_t *);
	sdb_object_t       *ud   = va_arg(ap, sdb_object_t *);

	assert(impl);

	if ((! impl->store_host) || (! impl->store_service)
			|| (! impl->store_metric) || (! impl->store_attribute)
			|| (! impl->store_service_attr) || (! impl->store_metric_attr)) {
		sdb_log(SDB_LOG_ERR, "core: store writer callback '%s' "
				"does not fully implement the writer interface.",
				obj->name);
		return -1;
	}
	if (sdb_llist_search_by_name(writer_list, obj->name)) {
		sdb_log(SDB_LOG_WARNING, "core: store writer callback '%s' "
				"has already been registered. Ignoring newly "
				"registered version.", obj->name);
		return -1;
	}

	/* ctx may be NULL if the plugin was not registered by a plugin */

	SDB_PLUGIN_WRITER(obj)->impl = *impl;
	SDB_PLUGIN_WRITER(obj)->w_ctx  = ctx_get();
	sdb_object_ref(SDB_OBJ(SDB_PLUGIN_WRITER(obj)->w_ctx));

	sdb_object_ref(ud);
	SDB_PLUGIN_WRITER(obj)->w_user_data = ud;
	return 0;
} /* plugin_writer_init */

static void
plugin_writer_destroy(sdb_object_t *obj)
{
	assert(obj);
	sdb_object_deref(SDB_PLUGIN_WRITER(obj)->w_user_data);
	sdb_object_deref(SDB_OBJ(SDB_PLUGIN_WRITER(obj)->w_ctx));
} /* plugin_writer_destroy */

static sdb_type_t sdb_plugin_writer_type = {
	sizeof(sdb_plugin_writer_t),

	plugin_writer_init,
	plugin_writer_destroy
};

static int
module_init(const char *name, lt_dlhandle lh, sdb_plugin_info_t *info)
{
	int (*mod_init)(sdb_plugin_info_t *);
	int status;

	mod_init = (int (*)(sdb_plugin_info_t *))lt_dlsym(lh, "sdb_module_init");
	if (! mod_init) {
		sdb_log(SDB_LOG_ERR, "core: Failed to load plugin '%s': "
				"could not find symbol 'sdb_module_init'", name);
		return -1;
	}

	status = mod_init(info);
	if (status) {
		sdb_log(SDB_LOG_ERR, "core: Failed to initialize "
				"module '%s'", name);
		plugin_unregister_by_name(name);
		return -1;
	}
	return 0;
} /* module_init */

static int
module_load(const char *basedir, const char *name,
		const sdb_plugin_ctx_t *plugin_ctx)
{
	char  base_name[name ? strlen(name) + 1 : 1];
	const char *name_ptr;
	char *tmp;

	char filename[1024];
	lt_dlhandle lh;

	ctx_t *ctx;

	int status;

	assert(name);

	base_name[0] = '\0';
	name_ptr = name;

	while ((tmp = strstr(name_ptr, "::"))) {
		strncat(base_name, name_ptr, (size_t)(tmp - name_ptr));
		strcat(base_name, "/");
		name_ptr = tmp + strlen("::");
	}
	strcat(base_name, name_ptr);

	if (! basedir)
		basedir = PKGLIBDIR;

	snprintf(filename, sizeof(filename), "%s/%s.so", basedir, base_name);
	filename[sizeof(filename) - 1] = '\0';

	if (access(filename, R_OK)) {
		char errbuf[1024];
		sdb_log(SDB_LOG_ERR, "core: Failed to load plugin '%s' (%s): %s",
				name, filename, sdb_strerror(errno, errbuf, sizeof(errbuf)));
		return -1;
	}

	lt_dlinit();
	lt_dlerror();

	lh = lt_dlopen(filename);
	if (! lh) {
		sdb_log(SDB_LOG_ERR, "core: Failed to load plugin '%s': %s"
				"The most common cause for this problem are missing "
				"dependencies.\n", name, lt_dlerror());
		return -1;
	}

	if (ctx_get())
		sdb_log(SDB_LOG_WARNING, "core: Discarding old plugin context");

	ctx = ctx_create(name);
	if (! ctx) {
		sdb_log(SDB_LOG_ERR, "core: Failed to initialize plugin context");
		return -1;
	}

	ctx->info.plugin_name = strdup(name);
	ctx->info.filename = strdup(filename);
	ctx->handle = lh;

	if (plugin_ctx)
		ctx->public = *plugin_ctx;

	if ((status = module_init(name, lh, &ctx->info))) {
		sdb_object_deref(SDB_OBJ(ctx));
		return status;
	}

	/* compare minor version */
	if ((ctx->info.version < 0)
			|| ((int)(ctx->info.version / 100) != (int)(SDB_VERSION / 100)))
		sdb_log(SDB_LOG_WARNING, "core: WARNING: version of "
				"plugin '%s' (%i.%i.%i) does not match our version "
				"(%i.%i.%i); this might cause problems",
				name, SDB_VERSION_DECODE(ctx->info.version),
				SDB_VERSION_DECODE(SDB_VERSION));

	sdb_llist_append(all_plugins, SDB_OBJ(ctx));

	sdb_log(SDB_LOG_INFO, "core: Successfully loaded "
			"plugin %s v%i (%s)", ctx->info.plugin_name,
			ctx->info.plugin_version,
			INFO_GET(&ctx->info, description));
	sdb_log(SDB_LOG_INFO, "core: Plugin %s: %s, License: %s",
			ctx->info.plugin_name,
			INFO_GET(&ctx->info, copyright),
			INFO_GET(&ctx->info, license));

	/* any registered callbacks took ownership of the context */
	sdb_object_deref(SDB_OBJ(ctx));

	/* reset */
	ctx_set(NULL);
	return 0;
} /* module_load */

static char *
plugin_get_name(const char *name, char *buf, size_t bufsize)
{
	ctx_t *ctx = ctx_get();

	if (ctx)
		snprintf(buf, bufsize, "%s::%s", ctx->info.plugin_name, name);
	else
		snprintf(buf, bufsize, "core::%s", name);
	return buf;
} /* plugin_get_name */

static int
plugin_add_callback(sdb_llist_t **list, const char *type,
		const char *name, void *callback, sdb_object_t *user_data)
{
	sdb_object_t *obj;

	if ((! name) || (! callback))
		return -1;

	assert(list);

	if (! *list)
		*list = sdb_llist_create();
	if (! *list)
		return -1;

	obj = sdb_object_create(name, sdb_plugin_cb_type,
			list, type, callback, user_data);
	if (! obj)
		return -1;

	if (sdb_llist_append(*list, obj)) {
		sdb_object_deref(obj);
		return -1;
	}

	/* pass control to the list */
	sdb_object_deref(obj);

	sdb_log(SDB_LOG_INFO, "core: Registered %s callback '%s'.",
			type, name);
	return 0;
} /* plugin_add_callback */

/*
 * public API
 */

int
sdb_plugin_load(const char *basedir, const char *name,
		const sdb_plugin_ctx_t *plugin_ctx)
{
	ctx_t *ctx;

	int status;

	if ((! name) || (! *name))
		return -1;

	if (! all_plugins) {
		if (! (all_plugins = sdb_llist_create())) {
			sdb_log(SDB_LOG_ERR, "core: Failed to load plugin '%s': "
					"internal error while creating linked list", name);
			return -1;
		}
	}

	ctx = CTX(sdb_llist_search_by_name(all_plugins, name));
	if (ctx) {
		/* plugin already loaded */
		if (! ctx->use_cnt) {
			/* reloading plugin */
			ctx_t *old_ctx = ctx_set(ctx);

			status = module_init(ctx->info.plugin_name, ctx->handle, NULL);
			if (status)
				return status;

			sdb_log(SDB_LOG_INFO, "core: Successfully reloaded plugin "
					"'%s' (%s)", ctx->info.plugin_name,
					INFO_GET(&ctx->info, description));
			ctx_set(old_ctx);
		}
		++ctx->use_cnt;
		return 0;
	}

	return module_load(basedir, name, plugin_ctx);
} /* sdb_plugin_load */

int
sdb_plugin_set_info(sdb_plugin_info_t *info, int type, ...)
{
	va_list ap;

	if (! info)
		return -1;

	va_start(ap, type);

	switch (type) {
		case SDB_PLUGIN_INFO_DESC:
			{
				char *desc = va_arg(ap, char *);
				if (desc) {
					if (info->description)
						free(info->description);
					info->description = strdup(desc);
				}
			}
			break;
		case SDB_PLUGIN_INFO_COPYRIGHT:
			{
				char *copyright = va_arg(ap, char *);
				if (copyright)
					info->copyright = strdup(copyright);
			}
			break;
		case SDB_PLUGIN_INFO_LICENSE:
			{
				char *license = va_arg(ap, char *);
				if (license) {
					if (info->license)
						free(info->license);
					info->license = strdup(license);
				}
			}
			break;
		case SDB_PLUGIN_INFO_VERSION:
			{
				int version = va_arg(ap, int);
				info->version = version;
			}
			break;
		case SDB_PLUGIN_INFO_PLUGIN_VERSION:
			{
				int version = va_arg(ap, int);
				info->plugin_version = version;
			}
			break;
		default:
			va_end(ap);
			return -1;
	}

	va_end(ap);
	return 0;
} /* sdb_plugin_set_info */

int
sdb_plugin_register_config(sdb_plugin_config_cb callback)
{
	ctx_t *ctx = ctx_get();

	if (! ctx) {
		sdb_log(SDB_LOG_ERR, "core: Invalid attempt to register a "
				"config callback from outside a plugin");
		return -1;
	}
	return plugin_add_callback(&config_list, "config", ctx->info.plugin_name,
			(void *)callback, NULL);
} /* sdb_plugin_register_config */

int
sdb_plugin_register_init(const char *name, sdb_plugin_init_cb callback,
		sdb_object_t *user_data)
{
	char cb_name[1024];
	return plugin_add_callback(&init_list, "init",
			plugin_get_name(name, cb_name, sizeof(cb_name)),
			(void *)callback, user_data);
} /* sdb_plugin_register_init */

int
sdb_plugin_register_shutdown(const char *name, sdb_plugin_shutdown_cb callback,
		sdb_object_t *user_data)
{
	char cb_name[1024];
	return plugin_add_callback(&shutdown_list, "shutdown",
			plugin_get_name(name, cb_name, sizeof(cb_name)),
			(void *)callback, user_data);
} /* sdb_plugin_register_shutdown */

int
sdb_plugin_register_log(const char *name, sdb_plugin_log_cb callback,
		sdb_object_t *user_data)
{
	char cb_name[1024];
	return plugin_add_callback(&log_list, "log",
			plugin_get_name(name, cb_name, sizeof(cb_name)),
			callback, user_data);
} /* sdb_plugin_register_log */

int
sdb_plugin_register_cname(const char *name, sdb_plugin_cname_cb callback,
		sdb_object_t *user_data)
{
	char cb_name[1024];
	return plugin_add_callback(&cname_list, "cname",
			plugin_get_name(name, cb_name, sizeof(cb_name)),
			callback, user_data);
} /* sdb_plugin_register_cname */

int
sdb_plugin_register_collector(const char *name, sdb_plugin_collector_cb callback,
		const sdb_time_t *interval, sdb_object_t *user_data)
{
	char cb_name[1024];
	sdb_object_t *obj;

	if ((! name) || (! callback))
		return -1;

	if (! collector_list)
		collector_list = sdb_llist_create();
	if (! collector_list)
		return -1;

	plugin_get_name(name, cb_name, sizeof(cb_name));

	obj = sdb_object_create(cb_name, sdb_plugin_collector_cb_type,
			&collector_list, "collector", callback, user_data);
	if (! obj)
		return -1;

	if (interval)
		SDB_PLUGIN_CCB(obj)->ccb_interval = *interval;
	else {
		ctx_t *ctx = ctx_get();

		if (! ctx) {
			sdb_log(SDB_LOG_ERR, "core: Cannot determine interval "
					"for collector %s; none specified and no plugin "
					"context found", cb_name);
			return -1;
		}

		SDB_PLUGIN_CCB(obj)->ccb_interval = ctx->public.interval;
	}

	if (! (SDB_PLUGIN_CCB(obj)->ccb_next_update = sdb_gettime())) {
		char errbuf[1024];
		sdb_log(SDB_LOG_ERR, "core: Failed to determine current "
				"time: %s", sdb_strerror(errno, errbuf, sizeof(errbuf)));
		sdb_object_deref(obj);
		return -1;
	}

	if (sdb_llist_insert_sorted(collector_list, obj,
				plugin_cmp_next_update)) {
		sdb_object_deref(obj);
		return -1;
	}

	/* pass control to the list */
	sdb_object_deref(obj);

	sdb_log(SDB_LOG_INFO, "core: Registered collector callback '%s' "
			"(interval = %.3fs).", cb_name,
			SDB_TIME_TO_DOUBLE(SDB_PLUGIN_CCB(obj)->ccb_interval));
	return 0;
} /* sdb_plugin_register_collector */

int
sdb_plugin_register_ts_fetcher(const char *name,
		sdb_plugin_fetch_ts_cb callback, sdb_object_t *user_data)
{
	return plugin_add_callback(&ts_fetcher_list, "time-series fetcher",
			name, callback, user_data);
} /* sdb_plugin_register_ts_fetcher */

int
sdb_plugin_register_writer(const char *name,
		sdb_store_writer_t *writer, sdb_object_t *user_data)
{
	char cb_name[1024];
	sdb_object_t *obj;

	if ((! name) || (! writer))
		return -1;

	if (! writer_list)
		writer_list = sdb_llist_create();
	if (! writer_list)
		return -1;

	plugin_get_name(name, cb_name, sizeof(cb_name));

	obj = sdb_object_create(cb_name, sdb_plugin_writer_type,
			writer, user_data);
	if (! obj)
		return -1;

	if (sdb_llist_append(writer_list, obj)) {
		sdb_object_deref(obj);
		return -1;
	}

	/* pass control to the list */
	sdb_object_deref(obj);

	sdb_log(SDB_LOG_INFO, "core: Registered store writer callback '%s'.",
			cb_name);
	return 0;
} /* sdb_store_register_writer */

void
sdb_plugin_unregister_all(void)
{
	size_t i;

	for (i = 0; i < SDB_STATIC_ARRAY_LEN(all_lists); ++i) {
		const char  *type =  all_lists[i].type;
		sdb_llist_t *list = *all_lists[i].list;

		size_t len = sdb_llist_len(list);

		if (! len)
			continue;

		sdb_llist_clear(list);
		sdb_log(SDB_LOG_INFO, "core: Unregistered %zu %s callback%s",
				len, type, len == 1 ? "" : "s");
	}
} /* sdb_plugin_unregister_all */

sdb_plugin_ctx_t
sdb_plugin_get_ctx(void)
{
	ctx_t *c;

	c = ctx_get();
	if (! c) {
		sdb_plugin_log(SDB_LOG_ERR, "core: Invalid read access to plugin "
				"context outside a plugin");
		return plugin_default_ctx;
	}
	return c->public;
} /* sdb_plugin_get_ctx */

int
sdb_plugin_set_ctx(sdb_plugin_ctx_t ctx, sdb_plugin_ctx_t *old)
{
	ctx_t *c;

	c = ctx_get();
	if (! c) {
		sdb_plugin_log(SDB_LOG_ERR, "core: Invalid write access to plugin "
				"context outside a plugin");
		return -1;
	}

	if (old)
		*old = c->public;
	c->public = ctx;
	return 0;
} /* sdb_plugin_set_ctx */

const sdb_plugin_info_t *
sdb_plugin_current(void)
{
	ctx_t *ctx = ctx_get();

	if (! ctx)
		return NULL;
	return &ctx->info;
} /* sdb_plugin_current */

int
sdb_plugin_configure(const char *name, oconfig_item_t *ci)
{
	sdb_plugin_cb_t *plugin;
	sdb_plugin_config_cb callback;

	ctx_t *old_ctx;

	int status;

	if ((! name) || (! ci))
		return -1;

	plugin = SDB_PLUGIN_CB(sdb_llist_search_by_name(config_list, name));
	if (! plugin) {
		ctx_t *ctx = CTX(sdb_llist_search_by_name(all_plugins, name));
		if (! ctx)
			sdb_log(SDB_LOG_ERR, "core: Cannot configure unknown "
					"plugin '%s'. Missing 'LoadPlugin \"%s\"'?",
					name, name);
		else
			sdb_log(SDB_LOG_ERR, "core: Plugin '%s' did not register "
					"a config callback.", name);
		errno = ENOENT;
		return -1;
	}

	old_ctx = ctx_set(plugin->cb_ctx);
	callback = (sdb_plugin_config_cb)plugin->cb_callback;
	status = callback(ci);
	ctx_set(old_ctx);
	return status;
} /* sdb_plugin_configure */

int
sdb_plugin_reconfigure_init(void)
{
	sdb_llist_iter_t *iter;

	iter = sdb_llist_get_iter(config_list);
	if (config_list && (! iter))
		return -1;

	/* deconfigure all plugins */
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_cb_t *plugin;
		sdb_plugin_config_cb callback;
		ctx_t *old_ctx;

		plugin = SDB_PLUGIN_CB(sdb_llist_iter_get_next(iter));
		old_ctx = ctx_set(plugin->cb_ctx);
		callback = (sdb_plugin_config_cb)plugin->cb_callback;
		callback(NULL);
		ctx_set(old_ctx);
	}
	sdb_llist_iter_destroy(iter);

	iter = sdb_llist_get_iter(all_plugins);
	if (all_plugins && (! iter))
		return -1;

	/* record all plugins as being unused */
	while (sdb_llist_iter_has_next(iter))
		CTX(sdb_llist_iter_get_next(iter))->use_cnt = 0;
	sdb_llist_iter_destroy(iter);

	sdb_plugin_unregister_all();
	return 0;
} /* sdb_plugin_reconfigure_init */

int
sdb_plugin_reconfigure_finish(void)
{
	sdb_llist_iter_t *iter;

	iter = sdb_llist_get_iter(all_plugins);
	if (all_plugins && (! iter))
		return -1;

	while (sdb_llist_iter_has_next(iter)) {
		ctx_t *ctx = CTX(sdb_llist_iter_get_next(iter));
		if (ctx->use_cnt)
			continue;

		sdb_log(SDB_LOG_INFO, "core: Module %s no longer in use",
				ctx->info.plugin_name);
		sdb_llist_iter_remove_current(iter);
		plugin_unregister_by_name(ctx->info.plugin_name);
		sdb_object_deref(SDB_OBJ(ctx));
	}
	sdb_llist_iter_destroy(iter);
	return 0;
} /* sdb_plugin_reconfigure_finish */

int
sdb_plugin_init_all(void)
{
	sdb_llist_iter_t *iter;
	int ret = 0;

	iter = sdb_llist_get_iter(init_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_cb_t *cb;
		sdb_plugin_init_cb callback;
		ctx_t *old_ctx;

		sdb_object_t *obj = sdb_llist_iter_get_next(iter);
		assert(obj);
		cb = SDB_PLUGIN_CB(obj);

		callback = (sdb_plugin_init_cb)cb->cb_callback;

		old_ctx = ctx_set(cb->cb_ctx);
		if (callback(cb->cb_user_data)) {
			sdb_log(SDB_LOG_ERR, "core: Failed to initialize plugin "
					"'%s'. Unregistering all callbacks.", obj->name);
			ctx_set(old_ctx);
			plugin_unregister_by_name(cb->cb_ctx->info.plugin_name);
			++ret;
		}
		else
			ctx_set(old_ctx);
	}
	sdb_llist_iter_destroy(iter);
	return ret;
} /* sdb_plugin_init_all */

int
sdb_plugin_shutdown_all(void)
{
	sdb_llist_iter_t *iter;
	int ret = 0;

	iter = sdb_llist_get_iter(shutdown_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_cb_t *cb;
		sdb_plugin_shutdown_cb callback;
		ctx_t *old_ctx;

		sdb_object_t *obj = sdb_llist_iter_get_next(iter);
		assert(obj);
		cb = SDB_PLUGIN_CB(obj);

		callback = (sdb_plugin_shutdown_cb)cb->cb_callback;

		old_ctx = ctx_set(cb->cb_ctx);
		if (callback(cb->cb_user_data)) {
			sdb_log(SDB_LOG_ERR, "core: Failed to shutdown plugin '%s'.",
					obj->name);
			++ret;
		}
		ctx_set(old_ctx);
	}
	sdb_llist_iter_destroy(iter);
	return ret;
} /* sdb_plugin_shutdown_all */

int
sdb_plugin_collector_loop(sdb_plugin_loop_t *loop)
{
	if (! collector_list) {
		sdb_log(SDB_LOG_WARNING, "core: No collectors registered. "
				"Quiting main loop.");
		return -1;
	}

	if (! loop)
		return -1;

	while (loop->do_loop) {
		sdb_plugin_collector_cb callback;
		ctx_t *old_ctx;

		sdb_time_t interval, now;

		sdb_object_t *obj = sdb_llist_shift(collector_list);
		if (! obj)
			return -1;

		callback = (sdb_plugin_collector_cb)SDB_PLUGIN_CCB(obj)->ccb_callback;

		if (! (now = sdb_gettime())) {
			char errbuf[1024];
			sdb_log(SDB_LOG_ERR, "core: Failed to determine current "
					"time in collector main loop: %s",
					sdb_strerror(errno, errbuf, sizeof(errbuf)));
			now = SDB_PLUGIN_CCB(obj)->ccb_next_update;
		}

		if (now < SDB_PLUGIN_CCB(obj)->ccb_next_update) {
			interval = SDB_PLUGIN_CCB(obj)->ccb_next_update - now;

			errno = 0;
			while (loop->do_loop && sdb_sleep(interval, &interval)) {
				if (errno != EINTR) {
					char errbuf[1024];
					sdb_log(SDB_LOG_ERR, "core: Failed to sleep "
							"in collector main loop: %s",
							sdb_strerror(errno, errbuf, sizeof(errbuf)));
					sdb_llist_insert_sorted(collector_list, obj,
							plugin_cmp_next_update);
					sdb_object_deref(obj);
					return -1;
				}
				errno = 0;
			}

			if (! loop->do_loop) {
				/* put back; don't worry about errors */
				sdb_llist_insert_sorted(collector_list, obj,
						plugin_cmp_next_update);
				sdb_object_deref(obj);
				return 0;
			}
		}

		old_ctx = ctx_set(SDB_PLUGIN_CCB(obj)->ccb_ctx);
		if (callback(SDB_PLUGIN_CCB(obj)->ccb_user_data)) {
			/* XXX */
		}
		ctx_set(old_ctx);

		interval = SDB_PLUGIN_CCB(obj)->ccb_interval;
		if (! interval)
			interval = loop->default_interval;
		if (! interval) {
			sdb_log(SDB_LOG_WARNING, "core: No interval configured "
					"for plugin '%s'; skipping any further "
					"iterations.", obj->name);
			sdb_object_deref(obj);
			continue;
		}

		SDB_PLUGIN_CCB(obj)->ccb_next_update += interval;

		if (! (now = sdb_gettime())) {
			char errbuf[1024];
			sdb_log(SDB_LOG_ERR, "core: Failed to determine current "
					"time in collector main loop: %s",
					sdb_strerror(errno, errbuf, sizeof(errbuf)));
			now = SDB_PLUGIN_CCB(obj)->ccb_next_update;
		}

		if (now > SDB_PLUGIN_CCB(obj)->ccb_next_update) {
			sdb_log(SDB_LOG_WARNING, "core: Plugin '%s' took too "
					"long; skipping iterations to keep up.",
					obj->name);
			SDB_PLUGIN_CCB(obj)->ccb_next_update = now;
		}

		if (sdb_llist_insert_sorted(collector_list, obj,
					plugin_cmp_next_update)) {
			sdb_log(SDB_LOG_ERR, "core: Failed to re-insert "
					"plugin '%s' into collector list. Unable to further "
					"use the plugin.",
					obj->name);
			sdb_object_deref(obj);
			return -1;
		}

		/* pass control back to the list */
		sdb_object_deref(obj);
	}
	return 0;
} /* sdb_plugin_read_loop */

char *
sdb_plugin_cname(char *hostname)
{
	sdb_llist_iter_t *iter;

	if (! hostname)
		return NULL;

	if (! cname_list)
		return hostname;

	iter = sdb_llist_get_iter(cname_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_cname_cb callback;
		char *cname;

		sdb_object_t *obj = sdb_llist_iter_get_next(iter);
		assert(obj);

		callback = (sdb_plugin_cname_cb)SDB_PLUGIN_CB(obj)->cb_callback;
		cname = callback(hostname, SDB_PLUGIN_CB(obj)->cb_user_data);
		if (cname) {
			free(hostname);
			hostname = cname;
		}
		/* else: don't change hostname */
	}
	sdb_llist_iter_destroy(iter);
	return hostname;
} /* sdb_plugin_cname */

int
sdb_plugin_log(int prio, const char *msg)
{
	sdb_llist_iter_t *iter;
	int ret = -1;

	bool logged = 0;

	if (! msg)
		return 0;

	iter = sdb_llist_get_iter(log_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_log_cb callback;
		int tmp;

		sdb_object_t *obj = sdb_llist_iter_get_next(iter);
		assert(obj);

		callback = (sdb_plugin_log_cb)SDB_PLUGIN_CB(obj)->cb_callback;
		tmp = callback(prio, msg, SDB_PLUGIN_CB(obj)->cb_user_data);
		if (tmp > ret)
			ret = tmp;

		if (SDB_PLUGIN_CB(obj)->cb_ctx)
			logged = 1;
		/* else: this is an internally registered callback */
	}
	sdb_llist_iter_destroy(iter);

	if (! logged)
		return fprintf(stderr, "[%s] %s\n", SDB_LOG_PRIO_TO_STRING(prio), msg);
	return ret;
} /* sdb_plugin_log */

int
sdb_plugin_vlogf(int prio, const char *fmt, va_list ap)
{
	sdb_strbuf_t *buf;
	int ret;

	if (! fmt)
		return 0;

	buf = sdb_strbuf_create(64);
	if (! buf) {
		ret = fprintf(stderr, "[%s] ", SDB_LOG_PRIO_TO_STRING(prio));
		ret += vfprintf(stderr, fmt, ap);
		return ret;
	}

	if (sdb_strbuf_vsprintf(buf, fmt, ap) < 0) {
		sdb_strbuf_destroy(buf);
		return -1;
	}

	ret = sdb_plugin_log(prio, sdb_strbuf_string(buf));
	sdb_strbuf_destroy(buf);
	return ret;
} /* sdb_plugin_vlogf */

int
sdb_plugin_logf(int prio, const char *fmt, ...)
{
	va_list ap;
	int ret;

	if (! fmt)
		return 0;

	va_start(ap, fmt);
	ret = sdb_plugin_vlogf(prio, fmt, ap);
	va_end(ap);
	return ret;
} /* sdb_plugin_logf */

sdb_timeseries_t *
sdb_plugin_fetch_timeseries(const char *type, const char *id,
		sdb_timeseries_opts_t *opts)
{
	sdb_plugin_cb_t *plugin;
	sdb_plugin_fetch_ts_cb callback;
	sdb_timeseries_t *ts;

	ctx_t *old_ctx;

	if ((! type) || (! id) || (! opts))
		return NULL;

	plugin = SDB_PLUGIN_CB(sdb_llist_search_by_name(ts_fetcher_list, type));
	if (! plugin) {
		sdb_log(SDB_LOG_ERR, "core: Cannot fetch time-series of type %s: "
				"no such plugin loaded", type);
		errno = ENOENT;
		return NULL;
	}

	old_ctx = ctx_set(plugin->cb_ctx);
	callback = (sdb_plugin_fetch_ts_cb)plugin->cb_callback;
	ts = callback(id, opts, plugin->cb_user_data);
	ctx_set(old_ctx);
	return ts;
} /* sdb_plugin_fetch_timeseries */

int
sdb_plugin_store_host(const char *name, sdb_time_t last_update)
{
	sdb_llist_iter_t *iter;
	int status = 0;

	if (! name)
		return -1;

	iter = sdb_llist_get_iter(writer_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_writer_t *writer;
		writer = SDB_PLUGIN_WRITER(sdb_llist_iter_get_next(iter));
		assert(writer);
		if (writer->impl.store_host(name, last_update, writer->w_user_data))
			status = -1;
	}
	sdb_llist_iter_destroy(iter);
	return status;
} /* sdb_plugin_store_host */

int
sdb_plugin_store_service(const char *hostname, const char *name,
		sdb_time_t last_update)
{
	sdb_llist_iter_t *iter;
	int status = 0;

	if ((! hostname) || (! name))
		return -1;

	iter = sdb_llist_get_iter(writer_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_writer_t *writer;
		writer = SDB_PLUGIN_WRITER(sdb_llist_iter_get_next(iter));
		assert(writer);
		if (writer->impl.store_service(hostname, name, last_update,
					writer->w_user_data))
			status = -1;
	}
	sdb_llist_iter_destroy(iter);
	return status;
} /* sdb_plugin_store_service */

int
sdb_plugin_store_metric(const char *hostname, const char *name,
		sdb_metric_store_t *store, sdb_time_t last_update)
{
	sdb_llist_iter_t *iter;
	int status = 0;

	if ((! hostname) || (! name))
		return -1;

	if ((! store->type) || (! store->id))
		store = NULL;

	iter = sdb_llist_get_iter(writer_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_writer_t *writer;
		writer = SDB_PLUGIN_WRITER(sdb_llist_iter_get_next(iter));
		assert(writer);
		if (writer->impl.store_metric(hostname, name, store, last_update,
					writer->w_user_data))
			status = -1;
	}
	sdb_llist_iter_destroy(iter);
	return status;
} /* sdb_plugin_store_metric */

int
sdb_plugin_store_attribute(const char *hostname, const char *key,
		const sdb_data_t *value, sdb_time_t last_update)
{
	sdb_llist_iter_t *iter;
	int status = 0;

	if ((! hostname) || (! key) || (! value))
		return -1;

	iter = sdb_llist_get_iter(writer_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_writer_t *writer;
		writer = SDB_PLUGIN_WRITER(sdb_llist_iter_get_next(iter));
		assert(writer);
		if (writer->impl.store_attribute(hostname, key, value, last_update,
					writer->w_user_data))
			status = -1;
	}
	sdb_llist_iter_destroy(iter);
	return status;
} /* sdb_plugin_store_attribute */

int
sdb_plugin_store_service_attribute(const char *hostname, const char *service,
		const char *key, const sdb_data_t *value, sdb_time_t last_update)
{
	sdb_llist_iter_t *iter;
	int status = 0;

	if ((! hostname) || (! service) || (! key) || (! value))
		return -1;

	iter = sdb_llist_get_iter(writer_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_writer_t *writer;
		writer = SDB_PLUGIN_WRITER(sdb_llist_iter_get_next(iter));
		assert(writer);
		if (writer->impl.store_service_attr(hostname, service,
					key, value, last_update, writer->w_user_data))
			status = -1;
	}
	sdb_llist_iter_destroy(iter);
	return status;
} /* sdb_plugin_store_service_attribute */

int
sdb_plugin_store_metric_attribute(const char *hostname, const char *metric,
		const char *key, const sdb_data_t *value, sdb_time_t last_update)
{
	sdb_llist_iter_t *iter;
	int status = 0;

	if ((! hostname) || (! metric) || (! key) || (! value))
		return -1;

	iter = sdb_llist_get_iter(writer_list);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_plugin_writer_t *writer;
		writer = SDB_PLUGIN_WRITER(sdb_llist_iter_get_next(iter));
		assert(writer);
		if (writer->impl.store_metric_attr(hostname, metric,
					key, value, last_update, writer->w_user_data))
			status = -1;
	}
	sdb_llist_iter_destroy(iter);
	return status;
} /* sdb_plugin_store_metric_attribute */

/* vim: set tw=78 sw=4 ts=4 noexpandtab : */

