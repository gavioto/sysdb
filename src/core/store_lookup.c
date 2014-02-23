/*
 * SysDB - src/core/store_lookup.c
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

/*
 * This module implements operators which may be used to select contents of
 * the store by matching various attributes of the stored objects. For now, a
 * simple full table scan is supported only.
 */

#include "sysdb.h"
#include "core/store-private.h"
#include "core/object.h"

#include <assert.h>

#include <sys/types.h>
#include <regex.h>

#include <stdlib.h>
#include <string.h>

/*
 * private data types
 */

/* match the name of something */
typedef struct {
	char    *name;
	regex_t *name_re;
} name_matcher_t;

/* matcher base type */
struct sdb_store_matcher {
	sdb_object_t super;
	/* type of the matcher */
	int type;
};
#define M(m) ((sdb_store_matcher_t *)(m))

/* logical operator matcher */
typedef struct {
	sdb_store_matcher_t super;

	/* left and right hand operands */
	sdb_store_matcher_t *left;
	sdb_store_matcher_t *right;
} op_matcher_t;
#define OP_M(m) ((op_matcher_t *)(m))

/* match any type of object by it's base information */
typedef struct {
	sdb_store_matcher_t super;

	/* match by the name of the object */
	name_matcher_t name;
} obj_matcher_t;
#define OBJ_M(m) ((obj_matcher_t *)(m))

/* match attributes */
typedef struct {
	obj_matcher_t super;
	/* XXX: this needs to be more flexible;
	 *      add support for type-specific operators */
	name_matcher_t value;
} attr_matcher_t;
#define ATTR_M(m) ((attr_matcher_t *)(m))

/* match services */
typedef struct {
	obj_matcher_t super;
	/* match by attributes assigned to the service */
	attr_matcher_t *attr;
} service_matcher_t;
#define SERVICE_M(m) ((service_matcher_t *)(m))

/* match hosts */
typedef struct {
	obj_matcher_t super;
	/* match by services assigned to the host */
	service_matcher_t *service;
	/* match by attributes assigned to the host */
	attr_matcher_t *attr;
} host_matcher_t;
#define HOST_M(m) ((host_matcher_t *)(m))

/*
 * matcher implementations
 */

static int
match_logical(sdb_store_matcher_t *m, sdb_store_base_t *obj);
static int
match_obj(sdb_store_matcher_t *m, sdb_store_base_t *obj);

/* specific matchers */

static int
match_name(name_matcher_t *m, const char *name)
{
	assert(m);

	if ((! m->name) && (! m->name_re))
		return 0;

	if (! name)
		name = "";

	if (m->name && strcasecmp(m->name, name))
		return -1;
	if (m->name_re && regexec(m->name_re, name,
					/* matches */ 0, NULL, /* flags = */ 0))
		return -1;
	return 0;
} /* match_name */

/* match attribute specific values;
 * always call this function through match_obj() */
static int
match_attr(attr_matcher_t *m, sdb_store_base_t *obj)
{
	assert(m && obj);

	if (obj->type != SDB_ATTRIBUTE)
		return -1;

	{
		sdb_attribute_t *attr = SDB_ATTR(obj);
		char buf[sdb_data_strlen(&attr->value) + 1];

		if (sdb_data_format(&attr->value, buf, sizeof(buf), SDB_UNQUOTED) <= 0)
			return -1;
		return match_name(&m->value, buf);
	}
} /* match_attr */

/* match service specific values;
 * always call this function through match_obj() */
static int
match_service(service_matcher_t *m, sdb_store_base_t *obj)
{
	sdb_llist_iter_t *iter;

	assert(m && obj);

	if (obj->type != SDB_SERVICE)
		return -1;

	if (! m->attr)
		return 0;

	iter = sdb_llist_get_iter(SDB_STORE_OBJ(obj)->attributes);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_store_base_t *attr = STORE_BASE(sdb_llist_iter_get_next(iter));

		/* if any of the attributes matches we found a matching service */
		if (! match_obj(M(m->attr), attr)) {
			sdb_llist_iter_destroy(iter);
			return 0;
		}
	}
	sdb_llist_iter_destroy(iter);
	return -1;
} /* match_service */

/* match host specific values;
 * always call this function through match_obj() */
static int
match_host(host_matcher_t *m, sdb_store_base_t *obj)
{
	sdb_llist_iter_t *iter;
	int status;

	assert(m && obj);

	if (obj->type != SDB_HOST)
		return -1;

	if (m->service) {
		iter = sdb_llist_get_iter(SDB_STORE_OBJ(obj)->children);
		status = -1;
	}
	else {
		iter = NULL;
		status = 0;
	}
	while (sdb_llist_iter_has_next(iter)) {
		sdb_store_base_t *service = STORE_BASE(sdb_llist_iter_get_next(iter));

		/* found a matching service */
		if (! match_obj(M(m->service), service)) {
			status = 0;
			break;
		}
	}
	sdb_llist_iter_destroy(iter);

	if (status)
		return status;
	else if (! m->attr)
		return 0;

	iter = sdb_llist_get_iter(SDB_STORE_OBJ(obj)->attributes);
	while (sdb_llist_iter_has_next(iter)) {
		sdb_store_base_t *attr = STORE_BASE(sdb_llist_iter_get_next(iter));

		/* if any attribute matches, we found a matching host */
		if (! match_obj(M(m->attr), attr)) {
			sdb_llist_iter_destroy(iter);
			return 0;
		}
	}
	sdb_llist_iter_destroy(iter);
	return -1;
} /* match_host */

/* generic matchers */

enum {
	MATCHER_OR,
	MATCHER_AND,
	MATCHER_ATTR,
	MATCHER_SERVICE,
	MATCHER_HOST,
};

typedef int (*matcher_cb)(sdb_store_matcher_t *, sdb_store_base_t *);

/* this array needs to be indexable by the matcher types */
static matcher_cb matchers[] = {
	match_logical,
	match_logical,
	match_obj,
	match_obj,
	match_obj,
};

static int
match_logical(sdb_store_matcher_t *m, sdb_store_base_t *obj)
{
	int status;

	assert(m && obj);
	assert(OP_M(m)->left && OP_M(m)->right);

	status = sdb_store_matcher_matches(OP_M(m)->left, obj);
	/* lazy evaluation */
	if (status && (m->type == MATCHER_AND))
		return status;
	else if ((! status) && (m->type == MATCHER_OR))
		return status;

	return sdb_store_matcher_matches(OP_M(m)->right, obj);
} /* match_logical */

static int
match_obj(sdb_store_matcher_t *m, sdb_store_base_t *obj)
{
	int status;

	assert(m && obj);

	status = match_name(&OBJ_M(m)->name, obj->super.name);
	if (status)
		return status;

	switch (m->type) {
		case MATCHER_ATTR:
			return match_attr(ATTR_M(m), obj);
			break;
		case MATCHER_SERVICE:
			return match_service(SERVICE_M(m), obj);
			break;
		case MATCHER_HOST:
			return match_host(HOST_M(m), obj);
			break;
	}
	return -1;
} /* match_obj */

/*
 * private matcher types
 */

/* initializes a name matcher consuming two elements from ap */
static int
name_matcher_init(name_matcher_t *m, va_list ap)
{
	const char *name = va_arg(ap, const char *);
	const char *name_re = va_arg(ap, const char *);

	if (name) {
		m->name = strdup(name);
		if (! m->name)
			return -1;
	}
	if (name_re) {
		m->name_re = malloc(sizeof(*m->name_re));
		if (! m->name_re)
			return -1;
		if (regcomp(m->name_re, name_re, REG_EXTENDED | REG_ICASE | REG_NOSUB))
			return -1;
	}
	return 0;
} /* name_matcher_init */

static void
name_matcher_destroy(name_matcher_t *m)
{
	if (m->name)
		free(m->name);
	if (m->name_re) {
		regfree(m->name_re);
		free(m->name_re);
	}
} /* name_matcher_destroy */

/* initializes an object matcher consuming two elements from ap */
static int
obj_matcher_init(sdb_object_t *obj, va_list ap)
{
	obj_matcher_t *m = OBJ_M(obj);
	return name_matcher_init(&m->name, ap);
} /* obj_matcher_init */

static void
obj_matcher_destroy(sdb_object_t *obj)
{
	obj_matcher_t *m = OBJ_M(obj);
	name_matcher_destroy(&m->name);
} /* obj_matcher_destroy */

static int
attr_matcher_init(sdb_object_t *obj, va_list ap)
{
	attr_matcher_t *attr = ATTR_M(obj);
	int status;

	M(obj)->type = MATCHER_ATTR;

	status = obj_matcher_init(obj, ap);
	if (! status)
		status = name_matcher_init(&attr->value, ap);
	return status;
} /* attr_matcher_init */

static void
attr_matcher_destroy(sdb_object_t *obj)
{
	attr_matcher_t *attr = ATTR_M(obj);

	obj_matcher_destroy(obj);
	name_matcher_destroy(&attr->value);
} /* attr_matcher_destroy */

static int
service_matcher_init(sdb_object_t *obj, va_list ap)
{
	attr_matcher_t *attr;
	int status;

	M(obj)->type = MATCHER_SERVICE;

	status = obj_matcher_init(obj, ap);
	if (status)
		return status;

	attr = va_arg(ap, attr_matcher_t *);

	sdb_object_ref(SDB_OBJ(attr));
	SERVICE_M(obj)->attr = attr;
	return 0;
} /* service_matcher_init */

static void
service_matcher_destroy(sdb_object_t *obj)
{
	obj_matcher_destroy(obj);
	sdb_object_deref(SDB_OBJ(SERVICE_M(obj)->attr));
} /* service_matcher_destroy */

static int
host_matcher_init(sdb_object_t *obj, va_list ap)
{
	service_matcher_t *service;
	attr_matcher_t *attr;
	int status;

	M(obj)->type = MATCHER_HOST;

	status = obj_matcher_init(obj, ap);
	if (status)
		return status;

	service = va_arg(ap, service_matcher_t *);
	attr = va_arg(ap, attr_matcher_t *);

	sdb_object_ref(SDB_OBJ(service));
	HOST_M(obj)->service = service;
	sdb_object_ref(SDB_OBJ(attr));
	HOST_M(obj)->attr = attr;
	return 0;
} /* host_matcher_init */

static void
host_matcher_destroy(sdb_object_t *obj)
{
	obj_matcher_destroy(obj);
	sdb_object_deref(SDB_OBJ(HOST_M(obj)->service));
	sdb_object_deref(SDB_OBJ(HOST_M(obj)->attr));
} /* host_matcher_destroy */

static int
op_matcher_init(sdb_object_t *obj, va_list ap)
{
	M(obj)->type = va_arg(ap, int);
	if ((M(obj)->type != MATCHER_OR) && (M(obj)->type != MATCHER_AND))
		return -1;

	OP_M(obj)->left = va_arg(ap, sdb_store_matcher_t *);
	sdb_object_ref(SDB_OBJ(OP_M(obj)->left));
	OP_M(obj)->right = va_arg(ap, sdb_store_matcher_t *);
	sdb_object_ref(SDB_OBJ(OP_M(obj)->right));

	if ((! OP_M(obj)->left) || (! OP_M(obj)->right))
		return -1;
	return 0;
} /* op_matcher_init */

static void
op_matcher_destroy(sdb_object_t *obj)
{
	if (OP_M(obj)->left)
		sdb_object_deref(SDB_OBJ(OP_M(obj)->left));
	if (OP_M(obj)->right)
		sdb_object_deref(SDB_OBJ(OP_M(obj)->right));
} /* op_matcher_destroy */

static sdb_type_t attr_type = {
	/* size = */ sizeof(attr_matcher_t),
	/* init = */ attr_matcher_init,
	/* destroy = */ attr_matcher_destroy,
};

static sdb_type_t service_type = {
	/* size = */ sizeof(service_matcher_t),
	/* init = */ service_matcher_init,
	/* destroy = */ service_matcher_destroy,
};

static sdb_type_t host_type = {
	/* size = */ sizeof(host_matcher_t),
	/* init = */ host_matcher_init,
	/* destroy = */ host_matcher_destroy,
};

static sdb_type_t op_type = {
	/* size = */ sizeof(op_matcher_t),
	/* init = */ op_matcher_init,
	/* destroy = */ op_matcher_destroy,
};

/*
 * public API
 */

sdb_store_matcher_t *
sdb_store_attr_matcher(const char *attr_name, const char *attr_name_re,
		const char *attr_value, const char *attr_value_re)
{
	return M(sdb_object_create("attr-matcher", attr_type,
				attr_name, attr_name_re, attr_value, attr_value_re));
} /* sdb_store_attr_matcher */

sdb_store_matcher_t *
sdb_store_service_matcher(const char *service_name, const char *service_name_re,
		sdb_store_matcher_t *attr_matcher)
{
	return M(sdb_object_create("service-matcher", service_type,
				service_name, service_name_re, attr_matcher));
} /* sdb_store_service_matcher */

sdb_store_matcher_t *
sdb_store_host_matcher(const char *host_name, const char *host_name_re,
		sdb_store_matcher_t *service_matcher,
		sdb_store_matcher_t *attr_matcher)
{
	return M(sdb_object_create("host-matcher", host_type,
				host_name, host_name_re, service_matcher, attr_matcher));
} /* sdb_store_host_matcher */

sdb_store_matcher_t *
sdb_store_dis_matcher(sdb_store_matcher_t *left, sdb_store_matcher_t *right)
{
	return M(sdb_object_create("dis-matcher", op_type, MATCHER_OR,
				left, right));
} /* sdb_store_dis_matcher */

sdb_store_matcher_t *
sdb_store_con_matcher(sdb_store_matcher_t *left, sdb_store_matcher_t *right)
{
	return M(sdb_object_create("con-matcher", op_type, MATCHER_AND,
				left, right));
} /* sdb_store_con_matcher */

int
sdb_store_matcher_matches(sdb_store_matcher_t *m, sdb_store_base_t *obj)
{
	/* "NULL" always matches */
	if ((! m) || (! obj))
		return 0;

	if ((m->type < 0) || ((size_t)m->type >= SDB_STATIC_ARRAY_LEN(matchers)))
		return -1;

	return matchers[m->type](m, obj);
} /* sdb_store_matcher_matches */

/* vim: set tw=78 sw=4 ts=4 noexpandtab : */

