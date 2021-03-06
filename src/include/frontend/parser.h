/*
 * SysDB - src/include/frontend/parser.h
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

#ifndef SDB_FRONTEND_PARSER_H
#define SDB_FRONTEND_PARSER_H 1

#include "core/store.h"
#include "frontend/connection.h"
#include "utils/llist.h"
#include "utils/strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* parser modes */
enum {
	/* parser accepts any command statement */
	SDB_PARSE_DEFAULT = 0,

	/* parser accepts any conditional statement */
	SDB_PARSE_COND    = 1 << 1,

	/* parser accepts any arithmetic expression */
	SDB_PARSE_ARITH   = 1 << 2,
};

/* YY_EXTRA data */
typedef struct {
	/* list of sdb_conn_node_t objects */
	sdb_llist_t *parsetree;

	/* parser mode */
	int mode;

	/* buffer for parser error messages */
	sdb_strbuf_t *errbuf;
} sdb_fe_yyextra_t;

/* see yyscan_t */
typedef void *sdb_fe_yyscan_t;

sdb_fe_yyscan_t
sdb_fe_scanner_init(const char *str, int len, sdb_fe_yyextra_t *yyext);

void
sdb_fe_scanner_destroy(sdb_fe_yyscan_t scanner);

int
sdb_fe_yyparse(sdb_fe_yyscan_t scanner);

sdb_store_matcher_t *
sdb_fe_parse_matcher(const char *cond, int len, sdb_strbuf_t *errbuf);

sdb_store_expr_t *
sdb_fe_parse_expr(const char *expr, int len, sdb_strbuf_t *errbuf);

/*
 * sdb_fe_analyze:
 * Analyze a parsed node, checking for semantical errors. Error messages will
 * be written to the string buffer, if provided.
 *
 * Returns:
 *  - 0 if the node is semantically correct
 *  - a negative value else
 */
int
sdb_fe_analyze(sdb_conn_node_t *node, sdb_strbuf_t *errbuf);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ! SDB_FRONTEND_PARSER_H */

/* vim: set tw=78 sw=4 ts=4 noexpandtab : */

