/*
 * SysDB - src/core/object.c
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

#include "core/object.h"

#include <assert.h>

#include <stdlib.h>
#include <string.h>

/*
 * private helper functions
 */

static int
sdb_object_wrapper_init(sdb_object_t *obj, va_list ap)
{
	void *data = va_arg(ap, void *);
	void (*destructor)(void *) = va_arg(ap, void (*)(void *));

	assert(obj);

	SDB_OBJ_WRAPPER(obj)->data = data;
	SDB_OBJ_WRAPPER(obj)->destructor = destructor;
	return 0;
} /* sdb_object_wrapper_init */

static void
sdb_object_wrapper_destroy(sdb_object_t *obj)
{
	if (! obj)
		return;

	assert(obj->ref_cnt <= 0);

	if (SDB_OBJ_WRAPPER(obj)->destructor && SDB_OBJ_WRAPPER(obj)->data)
		SDB_OBJ_WRAPPER(obj)->destructor(SDB_OBJ_WRAPPER(obj)->data);
	SDB_OBJ_WRAPPER(obj)->data = NULL;
} /* sdb_object_wrapper_destroy */

/*
 * public API
 */

sdb_object_t *
sdb_object_create(size_t size, int (*init)(sdb_object_t *, va_list),
		void (*destructor)(sdb_object_t *), ...)
{
	sdb_object_t *obj;

	obj = malloc(size);
	if (! obj)
		return NULL;
	memset(obj, 0, sizeof(*obj));

	if (init) {
		va_list ap;
		va_start(ap, destructor);

		if (init(obj, ap)) {
			obj->ref_cnt = 1;
			sdb_object_deref(obj);
			va_end(ap);
			return NULL;
		}

		va_end(ap);
	}

	obj->ref_cnt = 1;
	obj->destructor = destructor;
	obj->size = size;
	return obj;
} /* sdb_object_create */

sdb_object_t *
sdb_object_create_wrapper(void *data, void (*destructor)(void *))
{
	return sdb_object_create(sizeof(sdb_object_wrapper_t),
			sdb_object_wrapper_init, sdb_object_wrapper_destroy,
			data, destructor);
} /* sdb_object_create_wrapper */

void
sdb_object_deref(sdb_object_t *obj)
{
	if (! obj)
		return;

	--obj->ref_cnt;
	if (obj->ref_cnt > 0)
		return;

	if (obj->destructor)
		obj->destructor(obj);

	free(obj);
} /* sdb_object_deref */

void
sdb_object_ref(sdb_object_t *obj)
{
	if (! obj)
		return;
	assert(obj->ref_cnt > 0);
	++obj->ref_cnt;
} /* sdb_object_ref */

/* vim: set tw=78 sw=4 ts=4 noexpandtab : */
