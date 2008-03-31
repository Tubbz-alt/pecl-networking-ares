/*
    +--------------------------------------------------------------------+
    | PECL :: ares                                                       |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2006, Michael Wallner <mike@php.net>                 |
    +--------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_ares.h"

#include <ares.h>
#ifdef HAVE_ARES_VERSION
#	include <ares_version.h>
#endif
#ifdef HAVE_NETDB_H
#	include <netdb.h>
#endif
#ifdef HAVE_UNISTD_H
#	include <unistd.h>
#endif
#ifdef HAVE_ARPA_INET_H
#	include <arpa/inet.h>
#endif
#ifdef HAVE_ARPA_NAMESER_H
#	include <arpa/nameser.h>
#endif

#define local inline

#ifndef ZEND_ENGINE_2
#	define zend_is_callable(a,b,c) 1
#	ifndef ZTS
#		undef TSRMLS_SET_CTX
#		define TSRMLS_SET_CTX
#		undef TSRMLS_FETCH_FROM_CTX
#		define TSRMLS_FETCH_FROM_CTX
#	endif
#endif

#define PHP_ARES_LE_NAME "AsyncResolver"
#define PHP_ARES_QUERY_LE_NAME "AsyncResolverQuery"
static int le_ares;
static int le_ares_query;

#ifdef HAVE_OLD_ARES_STRERROR
#	define PHP_ARES_ERROR(err) { \
	char *__tmp = NULL; \
	php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", ares_strerror(err, &__tmp)); \
	if (__tmp) ares_free_errmem(__tmp); \
}
#else
#	define PHP_ARES_ERROR(err) \
	php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", ares_strerror(err))
#endif

#define RETURN_ARES_ERROR(err) \
	PHP_ARES_ERROR(err); \
	RETURN_FALSE
#define PHP_ARES_CB_ERROR(param) \
	php_error_docref(NULL TSRMLS_CC, E_WARNING, "Expected the " param " argument to be a valid callback")
#define RETURN_ARES_CB_ERROR(param) \
	PHP_ARES_CB_ERROR(param); \
	RETURN_FALSE

/* {{{ typedefs */
typedef struct _php_ares_options {
	struct ares_options strct;
	int flags;
} php_ares_options;

typedef struct _php_ares {
	ares_channel channel;
	php_ares_options options;
	zend_llist queries;
	void ***tsrm_ls;
	unsigned in_callback:1;
	unsigned reserved:31;
} php_ares;

typedef enum _php_ares_query_type {
	PHP_ARES_CB_STD,
	PHP_ARES_CB_HOST,
	PHP_ARES_CB_NINFO,
} php_ares_query_type;

typedef enum _php_ares_query_packet_type {
	PHP_ARES_PCKT_SEARCH,
	PHP_ARES_PCKT_QUERY,
	PHP_ARES_PCKT_SEND,
	PHP_ARES_PCKT_HNAME,
	PHP_ARES_PCKT_HADDR,
	PHP_ARES_PCKT_NINFO,
} php_ares_query_packet_type;

typedef union _php_ares_query_packet_data {
	struct {
		char *name;
		int name_len;
		long type;
		long dnsclass;
	} search;
	struct {
		char *name;
		int name_len;
		long type;
		long dnsclass;
	} query;
	struct {
		char *buf;
		int len;
	} send;
	struct {
		char *name;
		int name_len;
		long family;
	} hname;
	struct {
		char *addr;
		int addr_len;
		long family;
	} haddr;
	struct {
		char *addr;
		int addr_len;
		long port;
		long family;
		long flags;
	} ninfo;
} php_ares_query_packet_data;

typedef struct _php_ares_query_packet {
	php_ares_query_packet_type type;
	php_ares_query_packet_data data;
} php_ares_query_packet;

typedef union _php_ares_query_result {
	struct {
		char *buf;
		int len;
	} std;
	struct hostent host;
	struct {
		char *service;
		char *node;
	} ninfo;
} php_ares_query_result;

typedef struct _php_ares_query {
	int id;
	int error;
	php_ares *ares;
	zval *callback;
	php_ares_query_type type;
	php_ares_query_packet packet;
	php_ares_query_result result;
} php_ares_query;
/* }}} */

local struct hostent *php_ares_hostent_ctor(struct hostent *host) /* {{{ */
{
	if (!host) {
		host = emalloc(sizeof(struct hostent));
	}
	memset(host, 0, sizeof(struct hostent));
	
	return host;
}
/* }}} */

local void php_ares_hostent_copy(struct hostent *from, struct hostent *to) /* {{{ */
{
	int i, c;
	char **ptr;
	
	memcpy(to, from, sizeof(struct hostent));
	to->h_name = estrdup(from->h_name);
	for (c = 0, ptr = from->h_aliases; *ptr; ++ptr, ++c);
	to->h_aliases = ecalloc((c+1), sizeof(char *));
	for (i = 0; i < c; ++i) {
		to->h_aliases[i] = estrdup(from->h_aliases[i]);
	}
	for (c = 0, ptr = from->h_addr_list; *ptr; ++ptr, ++c);
	to->h_addr_list = ecalloc((c+1), sizeof(char *));
	for (i = 0; i < c; ++i) {
		to->h_addr_list[i] = emalloc(from->h_length);
		memcpy(to->h_addr_list[i], from->h_addr_list[i], from->h_length);
	}
}
/* }}} */

local void php_ares_hostent_to_struct(struct hostent *hostent, HashTable *ht) /* {{{ */
{
	zval array, *tmp;
	char **ptr;
	
	INIT_PZVAL(&array);
	Z_TYPE(array) = IS_ARRAY;
	Z_ARRVAL(array) = ht;
	
	if (hostent) {
		add_assoc_string(&array, "name", hostent->h_name, 1);
		
		MAKE_STD_ZVAL(tmp);
		array_init(tmp);
		if (hostent->h_aliases) {
			for (ptr = hostent->h_aliases; *ptr; ++ptr) {
				add_next_index_string(tmp, *ptr, 1);
			}
		}
		add_assoc_zval(&array, "aliases", tmp);
		add_assoc_long(&array, "addrtype", hostent->h_addrtype);
		
		MAKE_STD_ZVAL(tmp);
		array_init(tmp);
		if (hostent->h_addr_list) {
			for (ptr = hostent->h_addr_list; *ptr; ++ptr) {
				char name[64] = {0};
				
				if (inet_ntop(hostent->h_addrtype, *ptr, name, sizeof(name)-1)) {
					add_next_index_string(tmp, name, 1);
				}
			}
		}
		add_assoc_zval(&array, "addrlist", tmp);
	}
}
/* }}} */

local void php_ares_hostent_dtor(struct hostent *host) /* {{{ */
{
	char **ptr;
	
	STR_FREE(host->h_name);
	if (host->h_aliases) {
		for (ptr = host->h_aliases; *ptr; ++ptr) {
			efree(*ptr);
		}
		efree(host->h_aliases);
	}
	if (host->h_addr_list) {
		for (ptr = host->h_addr_list; *ptr; ++ptr) {
			efree(*ptr);
		}
		efree(host->h_addr_list);
	}
	memset(host, 0, sizeof(struct hostent));
}
/* }}} */

local void php_ares_hostent_free(struct hostent **host) /* {{{ */
{
	php_ares_hostent_dtor(*host);
	efree(*host);
	*host = NULL;
}
/* }}} */

local php_ares_query *php_ares_query_ctor(php_ares_query *query, php_ares_query_type type, php_ares *ares, zval *callback) /* {{{ */
{
	if (!query) {
		query = emalloc(sizeof(php_ares_query));
	}
	memset(query, 0, sizeof(php_ares_query));
	
	query->ares = ares;
	query->type = type;
	query->error = -1;
	
	if (callback) {
		ZVAL_ADDREF(callback);
		query->callback = callback;
	}
	
	return query;
}
/* }}} */

local void php_ares_query_rsrc(php_ares_query *query, zval *return_value) /* {{{ */
{
	TSRMLS_FETCH_FROM_CTX(query->ares->tsrm_ls);
	
	ZEND_REGISTER_RESOURCE(return_value, query, le_ares_query);
	query->id = Z_LVAL_P(return_value);
	zend_list_addref(query->id);
	zend_llist_add_element(&query->ares->queries, &query);
}
/* }}} */

local void php_ares_query_pckt(php_ares_query *query, php_ares_query_packet_type type, ...)
{
	va_list argv;
	char *buf;
	int len;
	
	va_start(argv, type);
	
	switch (query->packet.type = type) {
		case PHP_ARES_PCKT_SEARCH:
			buf = va_arg(argv, char *);
			len = va_arg(argv, int);
			query->packet.data.search.name = estrndup(buf, len);
			query->packet.data.search.name_len = len;
			query->packet.data.search.type = va_arg(argv, long);
			query->packet.data.search.dnsclass = va_arg(argv, long);
			break;
			
		case PHP_ARES_PCKT_QUERY:
			buf = va_arg(argv, char *);
			len = va_arg(argv, int);
			query->packet.data.query.name = estrndup(buf, len);
			query->packet.data.query.name_len = len;
			query->packet.data.query.type = va_arg(argv, long);
			query->packet.data.query.dnsclass = va_arg(argv, long);
			break;
			
		case PHP_ARES_PCKT_SEND:
			buf = va_arg(argv, char *);
			len = va_arg(argv, int);
			query->packet.data.send.buf = estrndup(buf, len);
			query->packet.data.send.len = len;
			break;
			
		case PHP_ARES_PCKT_HNAME:
			buf = va_arg(argv, char *);
			len = va_arg(argv, int);
			query->packet.data.hname.name = estrndup(buf, len);
			query->packet.data.hname.name_len = len;
			query->packet.data.hname.family = va_arg(argv, long);
			break;
			
		case PHP_ARES_PCKT_HADDR:
			buf = va_arg(argv, char *);
			len = va_arg(argv, int);
			query->packet.data.haddr.addr = estrndup(buf, len);
			query->packet.data.haddr.addr_len = len;
			query->packet.data.haddr.family = va_arg(argv, long);
			break;
			
		case PHP_ARES_PCKT_NINFO:
			query->packet.data.ninfo.flags = va_arg(argv, long);
			buf = va_arg(argv, char *);
			len = va_arg(argv, int);
			query->packet.data.ninfo.addr = estrndup(buf, len);
			query->packet.data.ninfo.addr_len = len;
			query->packet.data.ninfo.family = va_arg(argv, long);
			query->packet.data.ninfo.port = va_arg(argv, long);
			break;
	}
	
	va_end(argv);
}

local void php_ares_query_dtor(php_ares_query *query) /* {{{ */
{
	struct php_ares_query_packet_buf {char *buf;} *packet;
	
	packet = (struct php_ares_query_packet_buf *) &query->packet.data;
	if (packet->buf) {
		efree(packet->buf);
	}
	switch (query->type) {
		case PHP_ARES_CB_STD:
			STR_FREE(query->result.std.buf);
			break;
		case PHP_ARES_CB_HOST:
			php_ares_hostent_dtor(&query->result.host);
			break;
		case PHP_ARES_CB_NINFO:
			STR_FREE(query->result.ninfo.service);
			STR_FREE(query->result.ninfo.node);
			break;
	}
	if (query->callback) {
		zval_ptr_dtor(&query->callback);
	}
	memset(query, 0, sizeof(php_ares_query));
}
/* }}} */

local void php_ares_query_free(php_ares_query **query) /* {{{ */
{
	php_ares_query_dtor(*query);
	efree(*query);
	*query = NULL;
}
/* }}} */

local php_ares_options *php_ares_options_ctor(php_ares_options *options, HashTable *ht) /* {{{ */
{
	int i;
	zval **opt, **entry;
	
	if (!options) {
		options = emalloc(sizeof(php_ares_options));
	}
	memset(options, 0, sizeof(php_ares_options));
	
	if (ht && zend_hash_num_elements(ht)) {
		if ((SUCCESS == zend_hash_find(ht, "flags", sizeof("flags"), (void *) &opt)) && (Z_TYPE_PP(opt) == IS_LONG)) {
			options->flags |= ARES_OPT_FLAGS;
			options->strct.flags = Z_LVAL_PP(opt);
		}
		if ((SUCCESS == zend_hash_find(ht, "timeout", sizeof("timeout"), (void *) &opt)) && (Z_TYPE_PP(opt) == IS_LONG)) {
			options->flags |= ARES_OPT_TIMEOUT;
			options->strct.timeout = Z_LVAL_PP(opt);
		}
		if ((SUCCESS == zend_hash_find(ht, "tries", sizeof("tries"), (void *) &opt)) && (Z_TYPE_PP(opt) == IS_LONG)) {
			options->flags |= ARES_OPT_TRIES;
			options->strct.tries = Z_LVAL_PP(opt);
		}
		if ((SUCCESS == zend_hash_find(ht, "ndots", sizeof("ndots"), (void *) &opt)) && (Z_TYPE_PP(opt) == IS_LONG)) {
			options->flags |= ARES_OPT_NDOTS;
			options->strct.ndots = Z_LVAL_PP(opt);
		}
		if ((SUCCESS == zend_hash_find(ht, "udp_port", sizeof("udp_port"), (void *) &opt)) && (Z_TYPE_PP(opt) == IS_LONG)) {
			options->flags |= ARES_OPT_UDP_PORT;
			options->strct.udp_port = htons((unsigned short) Z_LVAL_PP(opt));
		}
		if ((SUCCESS == zend_hash_find(ht, "tcp_port", sizeof("tcp_port"), (void *) &opt)) && (Z_TYPE_PP(opt) == IS_LONG)) {
			options->flags |= ARES_OPT_TCP_PORT;
			options->strct.tcp_port = htons((unsigned short) Z_LVAL_PP(opt));
		}
		if ((SUCCESS == zend_hash_find(ht, "servers", sizeof("servers"), (void *) &opt)) && (Z_TYPE_PP(opt) == IS_ARRAY) && (i = zend_hash_num_elements(Z_ARRVAL_PP(opt)))) {
			options->strct.servers = ecalloc(i, sizeof(struct in_addr));
			for (	zend_hash_internal_pointer_reset(Z_ARRVAL_PP(opt));
					SUCCESS == zend_hash_get_current_data(Z_ARRVAL_PP(opt), (void *) &entry);
					zend_hash_move_forward(Z_ARRVAL_PP(opt))) {
				if (Z_TYPE_PP(entry) == IS_STRING) {
					inet_aton(Z_STRVAL_PP(entry), &options->strct.servers[options->strct.nservers++]);
				}
			}
			if (options->strct.nservers) {
				options->flags |= ARES_OPT_SERVERS;
			}
		}
		if ((SUCCESS == zend_hash_find(ht, "domains", sizeof("domains"), (void *) &opt)) && (Z_TYPE_PP(opt) == IS_ARRAY) && (i = zend_hash_num_elements(Z_ARRVAL_PP(opt)))) {
			options->strct.domains = ecalloc(i, sizeof(char *));
			for (	zend_hash_internal_pointer_reset(Z_ARRVAL_PP(opt));
					SUCCESS == zend_hash_get_current_data(Z_ARRVAL_PP(opt), (void *) &entry);
					zend_hash_move_forward(Z_ARRVAL_PP(opt))) {
				if (Z_TYPE_PP(entry) == IS_STRING) {
					options->strct.domains[options->strct.ndomains++] = estrdup(Z_STRVAL_PP(entry));
				}
			}
			if (options->strct.ndomains) {
				options->flags |= ARES_OPT_DOMAINS;
			}
		}
		if ((SUCCESS == zend_hash_find(ht, "lookups", sizeof("lookups"), (void *) &opt)) && (Z_TYPE_PP(opt) == IS_STRING)) {
			options->flags |= ARES_OPT_LOOKUPS;
			options->strct.lookups = estrdup(Z_STRVAL_PP(opt));
		}
	}
	
	return options;
}
/* }}} */

local void php_ares_options_dtor(php_ares_options *options) /* {{{ */
{
	int i;
	
	if (options->strct.servers) {
		efree(options->strct.servers);
	}
	
	if (options->strct.domains) {
		for (i = 0; i < options->strct.ndomains; ++i) {
			efree(options->strct.domains[i]);
		}
		efree(options->strct.domains);
	}
	
	STR_FREE(options->strct.lookups);
	
	memset(options, 0, sizeof(php_ares_options));
}
/* }}} */

local void php_ares_options_free(php_ares_options **options) /* {{{ */
{
	php_ares_options_dtor(*options);
	efree(*options);
	*options = NULL;
}
/* }}} */

/* {{{ callbacks */
static void php_ares_callback_func(void *aq, int status, unsigned char *abuf, int alen)
{
	php_ares_query *q = (php_ares_query *) aq;
	zval *params[3], *retval;
	TSRMLS_FETCH_FROM_CTX(q->ares->tsrm_ls);
	
	q->error = status;
	if (abuf) {
		q->result.std.buf = estrndup((char *) abuf, alen);
		q->result.std.len = alen;
	}
	
	if (q->callback) {
		MAKE_STD_ZVAL(retval);
		MAKE_STD_ZVAL(params[0]);
		MAKE_STD_ZVAL(params[1]);
		MAKE_STD_ZVAL(params[2]);
		ZVAL_NULL(retval);
		zend_list_addref(q->id);
		Z_LVAL_P(params[0]) = q->id;
		Z_TYPE_P(params[0]) = IS_RESOURCE;
		ZVAL_LONG(params[1], status);
		ZVAL_STRINGL(params[2], (char *) abuf, alen, 1);
	
		q->ares->in_callback = 1;
		call_user_function(EG(function_table), NULL, q->callback, retval, 3, params TSRMLS_CC);
		q->ares->in_callback = 0;
		
		zval_ptr_dtor(&retval);
		zval_ptr_dtor(&params[0]);
		zval_ptr_dtor(&params[1]);
		zval_ptr_dtor(&params[2]);
	}
}

static void php_ares_host_callback_func(void *aq, int status, struct hostent *hostent)
{
	php_ares_query *q = (php_ares_query *) aq;
	zval *params[3], *retval;
	TSRMLS_FETCH_FROM_CTX(q->ares->tsrm_ls);
	
	q->error = status;
	if (hostent) {
		php_ares_hostent_copy(hostent, &q->result.host);
	}
	
	if (q->callback) {
		MAKE_STD_ZVAL(retval);
		MAKE_STD_ZVAL(params[0]);
		MAKE_STD_ZVAL(params[1]);
		MAKE_STD_ZVAL(params[2]);
		ZVAL_NULL(retval);
		zend_list_addref(q->id);
		Z_LVAL_P(params[0]) = q->id;
		Z_TYPE_P(params[0]) = IS_RESOURCE;
		ZVAL_LONG(params[1], status);
		object_init(params[2]);
		php_ares_hostent_to_struct(hostent, HASH_OF(params[2]));
	
		q->ares->in_callback = 1;
		call_user_function(EG(function_table), NULL, q->callback, retval, 3, params TSRMLS_CC);
		q->ares->in_callback = 0;
	
		zval_ptr_dtor(&retval);
		zval_ptr_dtor(&params[0]);
		zval_ptr_dtor(&params[1]);
		zval_ptr_dtor(&params[2]);
	}
}

#ifdef HAVE_ARES_GETNAMEINFO
static void php_ares_nameinfo_callback_func(void *aq, int status, char *node, char *service)
{
	php_ares_query *q = (php_ares_query *) aq;
	zval *params[4], *retval;
	TSRMLS_FETCH_FROM_CTX(q->ares->tsrm_ls);
	
	q->error = status;
	if (node) {
		q->result.ninfo.node = estrdup(node);
	}
	if (service) {
		q->result.ninfo.service = estrdup(service);
	}
	
	if (q->callback) {
		MAKE_STD_ZVAL(retval);
		MAKE_STD_ZVAL(params[0]);
		MAKE_STD_ZVAL(params[1]);
		MAKE_STD_ZVAL(params[2]);
		MAKE_STD_ZVAL(params[3]);
		ZVAL_NULL(retval);
		zend_list_addref(q->id);
		Z_LVAL_P(params[0]) = q->id;
		Z_TYPE_P(params[0]) = IS_RESOURCE;
		ZVAL_LONG(params[1], status);
		if (node) {
			ZVAL_STRING(params[2], node, 1);
		} else {
			ZVAL_NULL(params[2]);
		}
		if (service) {
			ZVAL_STRING(params[3], service, 1);
		} else {
			ZVAL_NULL(params[3]);
		}
	
		q->ares->in_callback = 1;
		call_user_function(EG(function_table), NULL, q->callback, retval, 4, params TSRMLS_CC);
		q->ares->in_callback = 0;
		
		zval_ptr_dtor(&retval);
		zval_ptr_dtor(&params[0]);
		zval_ptr_dtor(&params[1]);
		zval_ptr_dtor(&params[2]);
		zval_ptr_dtor(&params[3]);
	}
}
#endif
/* }}} */

local struct timeval *php_ares_timeout(php_ares *ares, long max_timeout, struct timeval *tv_buf) /* {{{ */
{
	struct timeval maxtv;
	
	if (max_timeout > -1) {
		maxtv.tv_sec = max_timeout / 1000;
		maxtv.tv_usec = max_timeout % (max_timeout * 1000);
	}
	
	return ares_timeout(ares->channel, max_timeout > -1 ? &maxtv : NULL, tv_buf);
}
/* }}} */

local int php_ares_process(php_ares *ares, long max_timeout) /* {{{ */
{
	int nfds;
	fd_set R, W;
	struct timeval tv;
	
	FD_ZERO(&R);
	FD_ZERO(&W);
	
	if ((nfds = ares_fds(ares->channel, &R, &W))) {
		if (0 < select(nfds, &R, &W, NULL, php_ares_timeout(ares, max_timeout, &tv))) {
			ares_process(ares->channel, &R, &W);
		}
	}
	
	return nfds;
}
/* }}} */

local int php_ares_publish_fds(fd_set *R, fd_set *W, zval *r, zval *w) /* {{{ */
{
	int i, nfds = 0;
	
	for (i = 0; i < FD_SETSIZE; ++i) {
		if (FD_ISSET(i, R)) {
			add_next_index_long(r, i);
			if (i > nfds) {
				nfds = i;
			}
		}
	}
	
	for (i = 0; i < FD_SETSIZE; ++i) {
		if (FD_ISSET(i, W)) {
			add_next_index_long(w, i);
			if (i > nfds) {
				nfds = i;
			}
		}
	}
	
	return nfds ? nfds + 1 : 0;
}
/* }}} */

local int php_ares_extract_fds(zval *r, zval *w, fd_set *R, fd_set *W) /* {{{ */
{
	zval **fd;
	int nfds = 0;
	
	if (r && zend_hash_num_elements(Z_ARRVAL_P(r))) {
		for (	zend_hash_internal_pointer_reset(Z_ARRVAL_P(r));
				SUCCESS == zend_hash_get_current_data(Z_ARRVAL_P(r), (void *) &fd);
				zend_hash_move_forward(Z_ARRVAL_P(r))) {
			if (Z_TYPE_PP(fd) == IS_LONG) {
				FD_SET(Z_LVAL_PP(fd), R);
				if (Z_LVAL_PP(fd) > nfds) {
					nfds = Z_LVAL_PP(fd);
				}
			}
		}
	}
	
	if (w && zend_hash_num_elements(Z_ARRVAL_P(w))) {
		for (	zend_hash_internal_pointer_reset(Z_ARRVAL_P(w));
				SUCCESS == zend_hash_get_current_data(Z_ARRVAL_P(w), (void *) &fd);
				zend_hash_move_forward(Z_ARRVAL_P(w))) {
			if (Z_TYPE_PP(fd) == IS_LONG) {
				FD_SET(Z_LVAL_PP(fd), W);
				if (Z_LVAL_PP(fd) > nfds) {
					nfds = Z_LVAL_PP(fd);
				}
			}
		}
	}
	
	return nfds ? nfds + 1 : 0;
}
/* }}} */

static void php_ares_query_llist_dtor(void *entry)
{
	php_ares_query *q = *(php_ares_query **) entry;
	TSRMLS_FETCH_FROM_CTX(q->ares->tsrm_ls);
	zend_list_delete(q->id);
}

#ifdef HAVE_ARES_VERSION
/* {{{ proto string ares_version()
	Get libares version */
static PHP_FUNCTION(ares_version)
{
	if (ZEND_NUM_ARGS()) {
		WRONG_PARAM_COUNT;
	}
	
	RETURN_STRING(estrdup(ares_version(NULL)), 0);
}
/* }}} */
#endif

/* {{{ proto resource ares_init([array options])
	Create an ares resource */
static PHP_FUNCTION(ares_init)
{
	zval *opt_array = NULL;
	php_ares *ares = NULL;
	int err;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a!", &opt_array)) {
		RETURN_FALSE;
	}
	
	ares = emalloc(sizeof(php_ares));
	TSRMLS_SET_CTX(ares->tsrm_ls);
	zend_llist_init(&ares->queries, sizeof(php_ares_query *), (llist_dtor_func_t) php_ares_query_llist_dtor, 0);
	php_ares_options_ctor(&ares->options, opt_array ? Z_ARRVAL_P(opt_array) : NULL);
	
	if (ARES_SUCCESS != (err = ares_init_options(&ares->channel, &ares->options.strct, ares->options.flags))) {
		php_ares_options_dtor(&ares->options);
		zend_llist_destroy(&ares->queries);
		efree(ares);
		RETURN_ARES_ERROR(err);
	}
	
	ZEND_REGISTER_RESOURCE(return_value, ares, le_ares);
}
/* }}} */

/* {{{ proto void ares_destroy(resource ares)
	Destroy the ares handle */
static PHP_FUNCTION(ares_destroy)
{
	zval *rsrc;
	php_ares *ares;
	
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &rsrc)) {
		ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
		if (ares->in_callback) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot destroy ares handle while in callback");
		} else {
			zend_list_delete(Z_LVAL_P(rsrc));
		}
	}
}
/* }}} */

/* {{{ proto string ares_strerror(int status)
	Get description of status code */
static PHP_FUNCTION(ares_strerror)
{
	long err;
#ifdef HAVE_OLD_ARES_STRERROR
	char *__tmp = NULL;
	const char *__err;
#endif
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &err)) {
		RETURN_FALSE;
	}
	
#ifdef HAVE_OLD_ARES_STRERROR
	__err = ares_strerror(err, &__tmp);
	RETVAL_STRING(estrdup(__err), 0);
	if (__tmp) {
		ares_free_errmem(__tmp);
	}
#else
	RETURN_STRING(estrdup(ares_strerror(err)), 0);
#endif
}
/* }}} */

/* {{{ proto string ares_mkquery(string name, int dnsclass, int type, int id, int rd)
	Compose a custom query */
static PHP_FUNCTION(ares_mkquery)
{
	char *name_str, *query_str;
	int name_len, query_len, err;
	long dnsclass, type, id, rd;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sllll", &name_str, &name_len, &dnsclass, &type, &id, &rd)) {
		RETURN_FALSE;
	}
	
	if (ARES_SUCCESS != (err = ares_mkquery(name_str, dnsclass, type, id, rd, (unsigned char **) &query_str, &query_len))) {
		RETURN_ARES_ERROR(err);
	}
	RETVAL_STRINGL(query_str, query_len, 1);
	ares_free_string(query_str);
}
/* }}} */

/* {{{ proto resource ares_search(resource ares, mixed callback, string name[, int type = ARES_T_A[, int dnsclass = ARES_C_IN]])
	Issue a domain search for name */
static PHP_FUNCTION(ares_search)
{
	zval *rsrc, *cb = NULL;
	php_ares *ares;
	php_ares_query *query;
	char *name;
	int name_len;
	long dnsclass = ns_c_in, type = ns_t_a;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rz!s|ll", &rsrc, &cb, &name, &name_len, &type, &dnsclass)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	if (cb && !zend_is_callable(cb, 0, NULL)) {
		RETURN_ARES_CB_ERROR("second");
	}
	
	query = php_ares_query_ctor(NULL, PHP_ARES_CB_STD, ares, cb);
	php_ares_query_rsrc(query, return_value);
	php_ares_query_pckt(query, PHP_ARES_PCKT_SEARCH, name, name_len, type, dnsclass);
	ares_search(ares->channel, name, dnsclass, type, php_ares_callback_func, query);
}
/* }}} */

/* {{{ proto resource ares_query(resource ares, mixed callback, string name[, int type = ARES_T_A[, int dnsclass = ARES_C_IN]])
	Issue a single DNS query */
static PHP_FUNCTION(ares_query)
{
	zval *rsrc, *cb = NULL;
	php_ares *ares;
	php_ares_query *query;
	char *name;
	int name_len;
	long dnsclass = ns_c_in, type = ns_t_a;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rz!s|ll", &rsrc, &cb, &name, &name_len, &type, &dnsclass)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	if (cb && !zend_is_callable(cb, 0, NULL)) {
		RETURN_ARES_CB_ERROR("second");
	}
		
	query = php_ares_query_ctor(NULL, PHP_ARES_CB_STD, ares, cb);
	php_ares_query_rsrc(query, return_value);
	php_ares_query_pckt(query, PHP_ARES_PCKT_QUERY, name, name_len, type, dnsclass);
	ares_query(ares->channel, name, dnsclass, type, php_ares_callback_func, query);
}
/* }}} */

/* {{{ proto resource ares_send(resource ares, mixed callback, string buf)
	Send custom query */
static PHP_FUNCTION(ares_send)
{
	zval *rsrc, *cb = NULL;
	php_ares *ares;
	php_ares_query *query;
	char *buf;
	int len;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rz!s", &rsrc, &cb, &buf, &len)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	if (cb && !zend_is_callable(cb, 0, NULL)) {
		RETURN_ARES_CB_ERROR("second");
	}
	
	query = php_ares_query_ctor(NULL, PHP_ARES_CB_STD, ares, cb);
	php_ares_query_rsrc(query, return_value);
	php_ares_query_pckt(query, PHP_ARES_PCKT_SEND, buf, len);
	ares_send(ares->channel, (const unsigned char *) buf, len, php_ares_callback_func, query);
}
/* }}} */

/* {{{ proto resource ares_gethostbyname(resource ares, mixed callback, string name[, int family = AF_INET])
	Get host by name */
static PHP_FUNCTION(ares_gethostbyname)
{
	zval *rsrc, *cb = NULL;
	php_ares *ares;
	php_ares_query *query;
	char *name;
	int name_len;
	long family = AF_INET;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rz!s|l", &rsrc, &cb, &name, &name_len, &family)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	if (cb && !zend_is_callable(cb, 0, NULL)) {
		RETURN_ARES_CB_ERROR("second");
	}
	
	query = php_ares_query_ctor(NULL, PHP_ARES_CB_HOST, ares, cb);
	php_ares_query_rsrc(query, return_value);
	php_ares_query_pckt(query, PHP_ARES_PCKT_HNAME, name, name_len, family);
	ares_gethostbyname(ares->channel, name, family, php_ares_host_callback_func, query);
}
/* }}} */

/* {{{ proto resource ares_gethostbyaddr(resuorce ares, mixed callback, string address[, int family = ARES_AF_INET])
	Get host by address */
static PHP_FUNCTION(ares_gethostbyaddr)
{
	zval *rsrc, *cb = NULL;
	php_ares *ares;
	php_ares_query *query;
	char *addr;
	int addr_len;
	long family = AF_INET;
	void *sa;
	int sa_len;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rz!s|l", &rsrc, &cb, &addr, &addr_len, &family)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	if (cb && !zend_is_callable(cb, 0, NULL)) {
		PHP_ARES_CB_ERROR("second");
		RETURN_FALSE;
	}
	
	switch (family) {
		case AF_INET:
			sa = emalloc(sa_len = sizeof(struct in_addr));
			break;
		case AF_INET6:
			sa = emalloc(sa_len = sizeof(struct in6_addr));
			break;
		default:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Parameter family is neither ARES_AF_INET nor ARES_AF_INET6");
			RETURN_FALSE;
			break;
	}
	
	if (1 > inet_pton(family, addr, sa)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "inet_pton('%s') failed", addr);
		RETVAL_FALSE;
	} else {
		query = php_ares_query_ctor(NULL, PHP_ARES_CB_HOST, ares, cb);
		php_ares_query_rsrc(query, return_value);
		php_ares_query_pckt(query, PHP_ARES_PCKT_HADDR, addr, addr_len, family);
		ares_gethostbyaddr(ares->channel, sa, sa_len, family, php_ares_host_callback_func, query);
	}
	efree(sa);
}
/* }}} */

#ifdef HAVE_ARES_GETNAMEINFO
/* {{{ proto resource ares_getnameinfo(resource ares, mixed callback, int flags, string addr[, int family = ARES_AF_INET[, int port = 0]])
	Get name info */
static PHP_FUNCTION(ares_getnameinfo)
{
	zval *rsrc, *cb = NULL;
	php_ares *ares;
	php_ares_query *query;
	char *addr;
	int addr_len;
	long flags, port = 0, family = AF_INET;
	struct sockaddr *sa;
	struct sockaddr_in *in;
	struct sockaddr_in6 *in6;
	int sa_len;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rz!ls|ll", &rsrc, &cb, &flags, &addr, &addr_len, &family, &port)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	if (cb && !zend_is_callable(cb, 0, NULL)) {
		PHP_ARES_CB_ERROR("second");
		RETURN_FALSE;
	}
	
	RETVAL_TRUE;
	switch (family) {
		case AF_INET:
			in = ecalloc(1, sa_len = sizeof(struct sockaddr_in));
			in->sin_family = AF_INET;
			in->sin_port = htons((unsigned short) port);
			if (1 > inet_pton(in->sin_family, addr, &in->sin_addr)) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "inet_pton('%s') failed", addr);
				RETVAL_FALSE;
			}
			sa = (struct sockaddr *) in;
			break;
		case AF_INET6:
			in6 = ecalloc(1, sa_len = sizeof(struct sockaddr_in6));
			in6->sin6_family = AF_INET6;
			in6->sin6_port = htons((unsigned short) port);
			if (1 > inet_pton(in6->sin6_family, addr, &in6->sin6_addr)) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "inet_pton('%s') failed", addr);
				RETVAL_FALSE;
			}
			sa = (struct sockaddr *) in6;
			break;
		default:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Parameter family is neither AF_INET nor AF_INET6");
			RETURN_FALSE;
			break;
	}
	
	if (Z_BVAL_P(return_value)) {
		query = php_ares_query_ctor(NULL, PHP_ARES_CB_NINFO, ares, cb);
		php_ares_query_rsrc(query, return_value);
		php_ares_query_pckt(query, PHP_ARES_PCKT_NINFO, flags, addr, addr_len, family, port);
		ares_getnameinfo(ares->channel, sa, sa_len, flags, php_ares_nameinfo_callback_func, query);
	}
	efree(sa);
}
/* }}} */
#endif

/* {{{ proto mixed ares_result(resource query, int &errno, string &error)
	Check a query for its result */
static PHP_FUNCTION(ares_result)
{
	zval *rsrc, *zerrno = NULL, *zerror = NULL;
	php_ares_query *query;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|zz", &rsrc, &zerrno, &zerror)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(query, php_ares_query *, &rsrc, -1, PHP_ARES_QUERY_LE_NAME, le_ares_query);
	
	if (zerrno) {
		zval_dtor(zerrno);
		ZVAL_LONG(zerrno, query->error);
	}
	if (zerror) {
		zval_dtor(zerror);
		ZVAL_NULL(zerror);
	}
	
	switch (query->error) {
		case 0:
			switch (query->type) {
				case PHP_ARES_CB_STD:
					RETVAL_STRINGL(query->result.std.buf, query->result.std.len, 1);
					break;
				case PHP_ARES_CB_HOST:
					object_init(return_value);
					php_ares_hostent_to_struct(&query->result.host, HASH_OF(return_value));
					break;
				case PHP_ARES_CB_NINFO:
					object_init(return_value);
					add_property_string(return_value, "node", query->result.ninfo.node ? query->result.ninfo.node : "", 1);
					add_property_string(return_value, "service", query->result.ninfo.service ? query->result.ninfo.service : "", 1);
					break;
			}
			break;
		case -1:
			RETVAL_FALSE;
			break;
		default:
			if (zerror) {
#ifdef HAVE_OLD_ARES_STRERROR
				char *__tmp = NULL;
				const char *__err = ares_strerror(query->error, &__tmp);
				ZVAL_STRING(zerror, estrdup(__err), 0);
				if (__tmp) ares_free_errmem(__tmp);
#else
				ZVAL_STRING(zerror, estrdup(ares_strerror(query->error)), 0);
#endif
			}
			RETVAL_FALSE;
			break;
	}
}
/* }}} */

/* {{{ proto object ares_packet(resource query)
	Check a query for its question packet */
static PHP_FUNCTION(ares_packet)
{
	zval *rsrc, *prop;
	php_ares_query *query;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &rsrc)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(query, php_ares_query *, &rsrc, -1, PHP_ARES_QUERY_LE_NAME, le_ares_query);
	
	object_init(return_value);
	add_property_long(return_value, "type", query->packet.type);
	add_property_null(return_value, "search");
	add_property_null(return_value, "query");
	add_property_null(return_value, "send");
	add_property_null(return_value, "gethostbyname");
	add_property_null(return_value, "gethostbyaddr");
	add_property_null(return_value, "getnameinfo");
	MAKE_STD_ZVAL(prop);
	
	switch (query->packet.type) {
		case PHP_ARES_PCKT_SEARCH:
			object_init(prop);
			add_property_stringl(prop, "name", query->packet.data.search.name, query->packet.data.search.name_len, 1);
			add_property_long(prop, "type", query->packet.data.search.type);
			add_property_long(prop, "dnsclass", query->packet.data.search.dnsclass);
			add_property_zval(return_value, "search", prop);
			break;
			
		case PHP_ARES_PCKT_QUERY:
			object_init(prop);
			add_property_stringl(prop, "name", query->packet.data.query.name, query->packet.data.query.name_len, 1);
			add_property_long(prop, "type", query->packet.data.query.type);
			add_property_long(prop, "dnsclass", query->packet.data.query.dnsclass);
			add_property_zval(return_value, "query", prop);
			break;
			
		case PHP_ARES_PCKT_SEND:
			ZVAL_STRINGL(prop, query->packet.data.send.buf, query->packet.data.send.len, 1);
			add_property_zval(return_value, "send", prop);
			break;
			
		case PHP_ARES_PCKT_HNAME:
			object_init(prop);
			add_property_stringl(prop, "name", query->packet.data.hname.name, query->packet.data.hname.name_len, 1);
			add_property_long(prop, "family", query->packet.data.hname.family);
			add_property_zval(return_value, "gethostbyname", prop);
			break;
			
		case PHP_ARES_PCKT_HADDR:
			object_init(prop);
			add_property_stringl(prop, "addr", query->packet.data.haddr.addr, query->packet.data.haddr.addr_len, 1);
			add_property_long(prop, "family", query->packet.data.haddr.family);
			add_property_zval(return_value, "gethostbyaddr", prop);
			break;
			
		case PHP_ARES_PCKT_NINFO:
			object_init(prop);
			add_property_long(prop, "flags", query->packet.data.ninfo.flags);
			add_property_stringl(prop, "addr", query->packet.data.ninfo.addr, query->packet.data.ninfo.addr_len, 1);
			add_property_long(prop, "family", query->packet.data.ninfo.family);
			add_property_long(prop, "port", query->packet.data.ninfo.port);
			add_property_zval(return_value, "getnameinfo", prop);
			break;
	}
	
	zval_ptr_dtor(&prop);
}
/* }}} */

#ifdef HAVE_ARES_CANCEL
/* {{{ proto void ares_cancel(resource ares)
	Cancel pending queries */
static PHP_FUNCTION(ares_cancel)
{
	zval *rsrc;
	php_ares *ares;
	
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &rsrc)) {
		ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
		ares_cancel(ares->channel);
	}
}
/* }}} */
#endif

/* {{{ proto void ares_process_all(resource ares[, int max_timeout_ms])
	Process all pending queries */
static PHP_FUNCTION(ares_process_all)
{
	zval *rsrc;
	php_ares *ares;
	long max_timeout = -1;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l", &rsrc, &max_timeout)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	while (php_ares_process(ares, max_timeout));
}
/* }}} */

/* {{{ proto bool ares_process_once(resource ares[, int max_timout_ms])
	Process once and return whether it should be called again */
static PHP_FUNCTION(ares_process_once)
{
	zval *rsrc;
	php_ares *ares;
	long max_timeout = -1;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l", &rsrc, &max_timeout)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	RETVAL_BOOL(php_ares_process(ares, max_timeout));
}
/* }}} */

/* {{{ proto void ares_process(resource ares, array read, array write)
	Process call */
static PHP_FUNCTION(ares_process)
{
	zval *rsrc, *read = NULL, *write = NULL;
	fd_set R, W;
	php_ares *ares;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|a!a!", &rsrc, &read, &write)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	FD_ZERO(&R);
	FD_ZERO(&W);
	
	php_ares_extract_fds(read, write, &R, &W);
	ares_process(ares->channel, &R, &W);
}
/* }}} */

/* proto bool ares_select(array &read, array &write, int timeout_ms)
	Select call */
static PHP_FUNCTION(ares_select)
{
	zval *read = NULL, *write = NULL;
	fd_set R, W;
	int nfds;
	long timeout;
	struct timeval tv;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "aal", &read, &write, &timeout)) {
		RETURN_FALSE;
	}
	
	if (timeout) {
		tv.tv_sec = timeout / 1000;
		tv.tv_usec = timeout % (timeout * 1000);
	} else {
		tv.tv_sec = 1;
		tv.tv_usec = 0;
	}
	
	FD_ZERO(&R);
	FD_ZERO(&W);
	
	nfds = php_ares_extract_fds(read, write, &R, &W);
	if (-1 < select(nfds, &R, &W, NULL, &tv)) {
		zend_hash_clean(Z_ARRVAL_P(read));
		zend_hash_clean(Z_ARRVAL_P(write));
		php_ares_publish_fds(&R, &W, read, write);
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* proto int ares_timeout(resource ares[, int max_timout_ms])
	Get suggested select timeout in ms */
static PHP_FUNCTION(ares_timeout)
{
	zval *rsrc;
	long max_timeout = -1;
	struct timeval tv, *tvptr;
	php_ares *ares;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l", &rsrc, &max_timeout)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	if ((tvptr = php_ares_timeout(ares, max_timeout, &tv))) {
		RETURN_LONG(tvptr->tv_sec * 1000 + tvptr->tv_usec / 1000);
	}
	RETURN_LONG(0);
}
/* }}} */

/* {{{ proto int ares_fds(resource ares, array &read, array &write)
	Get file descriptors */
static PHP_FUNCTION(ares_fds)
{
	zval *rsrc, *read, *write;
	fd_set R, W;
	php_ares *ares;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rzz", &rsrc, &read, &write)) {
		RETURN_FALSE;
	}
	ZEND_FETCH_RESOURCE(ares, php_ares *, &rsrc, -1, PHP_ARES_LE_NAME, le_ares);
	
	FD_ZERO(&R);
	FD_ZERO(&W);
	
	zval_dtor(read);
	zval_dtor(write);
	array_init(read);
	array_init(write);
	ares_fds(ares->channel, &R, &W);
	RETVAL_LONG(php_ares_publish_fds(&R, &W, read, write));
}
/* }}} */


/* {{{ proto array ares_parse_a_reply(string reply)
	Parse an A reply */
static PHP_FUNCTION(ares_parse_a_reply)
{
	char *buf;
	int len, err;
	struct hostent *hostent;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &buf, &len)) {
		RETURN_FALSE;
	}
	
	if (ARES_SUCCESS != (err = ares_parse_a_reply((const unsigned char *) buf, len, &hostent))) {
		RETURN_ARES_ERROR(err);
	}
	
	object_init(return_value);
	php_ares_hostent_to_struct(hostent, HASH_OF(return_value));
	ares_free_hostent(hostent);
}
/* }}} */

#ifdef HAVE_ARES_PARSE_AAAA_REPLY
/* {{{ proto array ares_parse_aaaa_reply(string reply)
	Parse an AAAA reply */
static PHP_FUNCTION(ares_parse_aaaa_reply)
{
	char *buf;
	int len, err;
	struct hostent *hostent;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &buf, &len)) {
		RETURN_FALSE;
	}
	
	if (ARES_SUCCESS != (err = ares_parse_aaaa_reply((const unsigned char *) buf, len, &hostent))) {
		RETURN_ARES_ERROR(err);
	}
	
	object_init(return_value);
	php_ares_hostent_to_struct(hostent, HASH_OF(return_value));
	ares_free_hostent(hostent);
}
/* }}} */
#endif

/* {{{ proto array ares_parse_ptr_reply(string reply)
	Parse a PTR reply */
static PHP_FUNCTION(ares_parse_ptr_reply)
{
	char *buf;
	int len, err;
	struct hostent *hostent;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &buf, &len)) {
		RETURN_FALSE;
	}
	
	if (ARES_SUCCESS != (err = ares_parse_ptr_reply((const unsigned char *) buf, len, NULL, 0, 0, &hostent))) {
		RETURN_ARES_ERROR(err);
	}
	
	object_init(return_value);
	php_ares_hostent_to_struct(hostent, HASH_OF(return_value));
	ares_free_hostent(hostent);
}
/* }}} */

/* {{{ proto string ares_expand_name(string name)
	Expand a DNS encoded name into a human readable dotted string */
static PHP_FUNCTION(ares_expand_name)
{
	char *name_str, *exp_str;
	int name_len,err;
	PHP_ARES_EXPAND_LEN_TYPE exp_len;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &name_str, &name_len)) {
		RETURN_FALSE;
	}
	
	if (ARES_SUCCESS != (err = ares_expand_name((const unsigned char *) name_str, (const unsigned char *) name_str, name_len, &exp_str, &exp_len))) {
		RETURN_ARES_ERROR(err);
	}
	RETVAL_STRINGL(exp_str, exp_len, 1);
	ares_free_string(exp_str);
}
/* }}} */

#ifdef HAVE_ARES_EXPAND_STRING
/* {{{ proto string ares_expand_string(string buf)
	Expand a DNS encoded string into a human readable */
static PHP_FUNCTION(ares_expand_string)
{
	char *buf_str, *exp_str;
	int buf_len, err;
	PHP_ARES_EXPAND_LEN_TYPE exp_len;
	
	if (SUCCESS != zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &buf_str, &buf_len)) {
		RETURN_FALSE;
	}
	
	if (ARES_SUCCESS != (err = ares_expand_string((const unsigned char *) buf_str, (const unsigned char *) buf_str, buf_len, (unsigned char **) &exp_str, &exp_len))) {
		RETURN_ARES_ERROR(err);
	}
	RETVAL_STRINGL(exp_str, exp_len, 1);
	ares_free_string(exp_str);
}
/* }}} */
#endif

static ZEND_RSRC_DTOR_FUNC(php_ares_le_dtor)
{
	php_ares *ares = (php_ares *) rsrc->ptr;
	
	ares_destroy(ares->channel);
	zend_llist_destroy(&ares->queries);
	php_ares_options_dtor(&ares->options);
	efree(ares);
}

static ZEND_RSRC_DTOR_FUNC(php_ares_query_le_dtor)
{
	php_ares_query *query = (php_ares_query *) rsrc->ptr;
	
	php_ares_query_dtor(query);
	efree(query);
}

/* {{{ PHP_MINIT_FUNCTION */
static PHP_MINIT_FUNCTION(ares)
{
#ifdef HAVE_ARES_VERSION
	int ares_version_num;
	ares_version(&ares_version_num);
	
	REGISTER_LONG_CONSTANT("ARES_VERSION", ares_version_num, CONST_PERSISTENT|CONST_CS);
#endif
	
	REGISTER_LONG_CONSTANT("ARES_SUCCESS", ARES_SUCCESS, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_ENODATA", ARES_ENODATA, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_EFORMERR", ARES_EFORMERR, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_ESERVFAIL", ARES_ESERVFAIL, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_ENOTFOUND", ARES_ENOTFOUND, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_ENOTIMP", ARES_ENOTIMP, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_EREFUSED", ARES_EREFUSED, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_EBADQUERY", ARES_EBADQUERY, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_EBADNAME", ARES_EBADNAME, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_EBADFAMILY", ARES_EBADFAMILY, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_EBADRESP", ARES_EBADRESP, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_ECONNREFUSED", ARES_ECONNREFUSED, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_ETIMEOUT", ARES_ETIMEOUT, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_EOF", ARES_EOF, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_EFILE", ARES_EFILE, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_ENOMEM", ARES_ENOMEM, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_EDESTRUCTION", ARES_EDESTRUCTION, CONST_PERSISTENT|CONST_CS);
#ifdef ARES_EBADSTR
	REGISTER_LONG_CONSTANT("ARES_EBADSTR", ARES_EBADSTR, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_EBADFLAGS
	REGISTER_LONG_CONSTANT("ARES_EBADFLAGS", ARES_EBADFLAGS, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_ENONAME
	REGISTER_LONG_CONSTANT("ARES_ENONAME", ARES_ENONAME, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_EBADHINTS
	REGISTER_LONG_CONSTANT("ARES_EBADHINTS", ARES_EBADHINTS, CONST_PERSISTENT|CONST_CS);
#endif
	
	REGISTER_LONG_CONSTANT("ARES_FLAG_USEVC", ARES_FLAG_USEVC, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_FLAG_PRIMARY", ARES_FLAG_PRIMARY, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_FLAG_IGNTC", ARES_FLAG_IGNTC, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_FLAG_NORECURSE", ARES_FLAG_NORECURSE, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_FLAG_STAYOPEN", ARES_FLAG_STAYOPEN, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_FLAG_NOSEARCH", ARES_FLAG_NOSEARCH, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_FLAG_NOALIASES", ARES_FLAG_NOALIASES, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_FLAG_NOCHECKRESP", ARES_FLAG_NOCHECKRESP, CONST_PERSISTENT|CONST_CS);
	
	/*
	 * Address Family Constants
	 */
	REGISTER_LONG_CONSTANT("ARES_AF_INET", AF_INET, CONST_PERSISTENT|CONST_CS);
	REGISTER_LONG_CONSTANT("ARES_AF_INET6", AF_INET6, CONST_PERSISTENT|CONST_CS);
	
	/*
	 * Name Info constants
	 */
#ifdef ARES_NI_NOFQDN
	REGISTER_LONG_CONSTANT("ARES_NI_NOFQDN", ARES_NI_NOFQDN, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_NUMERICHOST
	REGISTER_LONG_CONSTANT("ARES_NI_NUMERICHOST", ARES_NI_NUMERICHOST, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_NAMEREQD
	REGISTER_LONG_CONSTANT("ARES_NI_NAMEREQD", ARES_NI_NAMEREQD, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_NUMERICSERV
	REGISTER_LONG_CONSTANT("ARES_NI_NUMERICSERV", ARES_NI_NUMERICSERV, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_DGRAM
	REGISTER_LONG_CONSTANT("ARES_NI_DGRAM", ARES_NI_DGRAM, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_TCP
	REGISTER_LONG_CONSTANT("ARES_NI_TCP", ARES_NI_TCP, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_UDP
	REGISTER_LONG_CONSTANT("ARES_NI_UDP", ARES_NI_UDP, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_SCTP
	REGISTER_LONG_CONSTANT("ARES_NI_SCTP", ARES_NI_SCTP, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_DCCP
	REGISTER_LONG_CONSTANT("ARES_NI_DCCP", ARES_NI_DCCP, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_NUMERICSCOPE
	REGISTER_LONG_CONSTANT("ARES_NI_NUMERICSCOPE", ARES_NI_NUMERICSCOPE, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_LOOKUPHOST
	REGISTER_LONG_CONSTANT("ARES_NI_LOOKUPHOST", ARES_NI_LOOKUPHOST, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_LOOKUPSERVICE
	REGISTER_LONG_CONSTANT("ARES_NI_LOOKUPSERVICE", ARES_NI_LOOKUPSERVICE, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_IDN
	REGISTER_LONG_CONSTANT("ARES_NI_IDN", ARES_NI_IDN, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_IDN_ALLOW_UNASSIGNED
	REGISTER_LONG_CONSTANT("ARES_NI_IDN_ALLOW_UNASSIGNED", ARES_NI_IDN_ALLOW_UNASSIGNED, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_NI_IDN_USE_STD
	REGISTER_LONG_CONSTANT("ARES_NI_IDN_USE_STD", ARES_NI_IDN_USE_STD, CONST_PERSISTENT|CONST_CS);
#endif
	
	/*
	 * Address Info constants
	 */
#ifdef ARES_AI_CANONNAME
	REGISTER_LONG_CONSTANT("ARES_AI_CANONNAME", ARES_AI_CANONNAME, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_NUMERICHOST
	REGISTER_LONG_CONSTANT("ARES_AI_NUMERICHOST", ARES_AI_NUMERICHOST, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_PASSIVE
	REGISTER_LONG_CONSTANT("ARES_AI_PASSIVE", ARES_AI_PASSIVE, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_NUMERICSERV
	REGISTER_LONG_CONSTANT("ARES_AI_NUMERICSERV", ARES_AI_NUMERICSERV, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_V
	REGISTER_LONG_CONSTANT("ARES_AI_V", ARES_AI_V, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_ALL
	REGISTER_LONG_CONSTANT("ARES_AI_ALL", ARES_AI_ALL, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_ADDRCONFIG
	REGISTER_LONG_CONSTANT("ARES_AI_ADDRCONFIG", ARES_AI_ADDRCONFIG, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_IDN
	REGISTER_LONG_CONSTANT("ARES_AI_IDN", ARES_AI_IDN, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_IDN_ALLOW_UNASSIGNED
	REGISTER_LONG_CONSTANT("ARES_AI_IDN_ALLOW_UNASSIGNED", ARES_AI_IDN_ALLOW_UNASSIGNED, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_IDN_USE_STD
	REGISTER_LONG_CONSTANT("ARES_AI_IDN_USE_STD", ARES_AI_IDN_USE_STD, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_CANONIDN
	REGISTER_LONG_CONSTANT("ARES_AI_CANONIDN", ARES_AI_CANONIDN, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_AI_MASK
	REGISTER_LONG_CONSTANT("ARES_AI_MASK", ARES_AI_MASK, CONST_PERSISTENT|CONST_CS);
#endif
#ifdef ARES_GETSOCK_MAXNUM
	REGISTER_LONG_CONSTANT("ARES_GETSOCK_MAXNUM", ARES_GETSOCK_MAXNUM, CONST_PERSISTENT|CONST_CS);
#endif
	
	/*
	 * ns_t (type) constants (arpa/nameser.h)
	 */

	/* (1)  Host address.  */
	REGISTER_LONG_CONSTANT("ARES_T_A", ns_t_a, CONST_CS|CONST_PERSISTENT);
	/* (2)  Authoritative server.  */
	REGISTER_LONG_CONSTANT("ARES_T_NS", ns_t_ns, CONST_CS|CONST_PERSISTENT);
	/* (3)  Mail destination.  */
	REGISTER_LONG_CONSTANT("ARES_T_MD", ns_t_md, CONST_CS|CONST_PERSISTENT);
	/* (4)  Mail forwarder.  */
	REGISTER_LONG_CONSTANT("ARES_T_MF", ns_t_mf, CONST_CS|CONST_PERSISTENT);
	/* (5)  Canonical name.  */
	REGISTER_LONG_CONSTANT("ARES_T_CNAME", ns_t_cname, CONST_CS|CONST_PERSISTENT);
	/* (6)  Start of authority zone.  */
	REGISTER_LONG_CONSTANT("ARES_T_SOA", ns_t_soa, CONST_CS|CONST_PERSISTENT);
	/* (7)  Mailbox domain name.  */
	REGISTER_LONG_CONSTANT("ARES_T_MB", ns_t_mb, CONST_CS|CONST_PERSISTENT);
	/* (8)  Mail group member.  */
	REGISTER_LONG_CONSTANT("ARES_T_MG", ns_t_mg, CONST_CS|CONST_PERSISTENT);
	/* (9)  Mail rename name.  */
	REGISTER_LONG_CONSTANT("ARES_T_MR", ns_t_mr, CONST_CS|CONST_PERSISTENT);
	/* (10)  Null resource record.  */
	REGISTER_LONG_CONSTANT("ARES_T_NULL", ns_t_null, CONST_CS|CONST_PERSISTENT);
	/* (11)  Well known service.  */
	REGISTER_LONG_CONSTANT("ARES_T_WKS", ns_t_wks, CONST_CS|CONST_PERSISTENT);
	/* (12)  Domain name pointer.  */
	REGISTER_LONG_CONSTANT("ARES_T_PTR", ns_t_ptr, CONST_CS|CONST_PERSISTENT);
	/* (13)  Host information.  */
	REGISTER_LONG_CONSTANT("ARES_T_HINFO", ns_t_hinfo, CONST_CS|CONST_PERSISTENT);
	/* (14)  Mailbox information.  */
	REGISTER_LONG_CONSTANT("ARES_T_MINFO", ns_t_minfo, CONST_CS|CONST_PERSISTENT);
	/* (15)  Mail routing information.  */
	REGISTER_LONG_CONSTANT("ARES_T_MX", ns_t_mx, CONST_CS|CONST_PERSISTENT);
	/* (16)  Text strings.  */
	REGISTER_LONG_CONSTANT("ARES_T_TXT", ns_t_txt, CONST_CS|CONST_PERSISTENT);
	/* (17)  Responsible person.  */
	REGISTER_LONG_CONSTANT("ARES_T_RP", ns_t_rp, CONST_CS|CONST_PERSISTENT);
	/* (18)  AFS cell database.  */
	REGISTER_LONG_CONSTANT("ARES_T_AFSDB", ns_t_afsdb, CONST_CS|CONST_PERSISTENT);
	/* (19)  X_25 calling address.  */
	REGISTER_LONG_CONSTANT("ARES_T_X25", ns_t_x25, CONST_CS|CONST_PERSISTENT);
	/* (20)  ISDN calling address.  */
	REGISTER_LONG_CONSTANT("ARES_T_ISDN", ns_t_isdn, CONST_CS|CONST_PERSISTENT);
	/* (21)  Router.  */
	REGISTER_LONG_CONSTANT("ARES_T_RT", ns_t_rt, CONST_CS|CONST_PERSISTENT);
	/* (22)  NSAP address.  */
	REGISTER_LONG_CONSTANT("ARES_T_NSAP", ns_t_nsap, CONST_CS|CONST_PERSISTENT);
	/* (23)  Reverse NSAP lookup (deprecated).  */
	/* REGISTER_LONG_CONSTANT("ARES_T_NSAP_PTR", ns_t_nsap_ptr, CONST_CS|CONST_PERSISTENT); */
	/* (24)  Security signature.  */
	REGISTER_LONG_CONSTANT("ARES_T_SIG", ns_t_sig, CONST_CS|CONST_PERSISTENT);
	/* (25)  Security key.  */
	REGISTER_LONG_CONSTANT("ARES_T_KEY", ns_t_key, CONST_CS|CONST_PERSISTENT);
	/* (26)  X.400 mail mapping.  */
	REGISTER_LONG_CONSTANT("ARES_T_PX", ns_t_px, CONST_CS|CONST_PERSISTENT);
	/* (27)  Geographical position (withdrawn).  */
	/* REGISTER_LONG_CONSTANT("ARES_T_GPOS", ns_t_gpos, CONST_CS|CONST_PERSISTENT); */
	/* (28)  Ip6 Address.  */
	REGISTER_LONG_CONSTANT("ARES_T_AAAA", ns_t_aaaa, CONST_CS|CONST_PERSISTENT);
	/* (29)  Location Information.  */
	REGISTER_LONG_CONSTANT("ARES_T_LOC", ns_t_loc, CONST_CS|CONST_PERSISTENT);
	/* (30)  Next domain (security).  */
	REGISTER_LONG_CONSTANT("ARES_T_NXT", ns_t_nxt, CONST_CS|CONST_PERSISTENT);
	/* (31)  Endpoint identifier.  */
	REGISTER_LONG_CONSTANT("ARES_T_EID", ns_t_eid, CONST_CS|CONST_PERSISTENT);
	/* (32)  Nimrod Locator.  */
	REGISTER_LONG_CONSTANT("ARES_T_NIMLOC", ns_t_nimloc, CONST_CS|CONST_PERSISTENT);
	/* (33)  Server Selection.  */
	REGISTER_LONG_CONSTANT("ARES_T_SRV", ns_t_srv, CONST_CS|CONST_PERSISTENT);
	/* (34)  ATM Address  */
	REGISTER_LONG_CONSTANT("ARES_T_ATMA", ns_t_atma, CONST_CS|CONST_PERSISTENT);
	/* (35)  Naming Authority PoinTeR  */
	REGISTER_LONG_CONSTANT("ARES_T_NAPTR", ns_t_naptr, CONST_CS|CONST_PERSISTENT);
	/* (36)  Key Exchange  */
	REGISTER_LONG_CONSTANT("ARES_T_KX", ns_t_kx, CONST_CS|CONST_PERSISTENT);
	/* (37)  Certification record  */
	REGISTER_LONG_CONSTANT("ARES_T_CERT", ns_t_cert, CONST_CS|CONST_PERSISTENT);
	/* (38)  IPv6 address (deprecates AAAA)  */
	REGISTER_LONG_CONSTANT("ARES_T_A6", ns_t_a6, CONST_CS|CONST_PERSISTENT);
	/* (39)  Non-terminal DNAME (for IPv6)  */
	REGISTER_LONG_CONSTANT("ARES_T_DNAME", ns_t_dname, CONST_CS|CONST_PERSISTENT);
	/* (40)  Kitchen sink (experimentatl)  */
	REGISTER_LONG_CONSTANT("ARES_T_SINK", ns_t_sink, CONST_CS|CONST_PERSISTENT);
	/* (41)  EDNS0 option (meta-RR)  */
	REGISTER_LONG_CONSTANT("ARES_T_OPT", ns_t_opt, CONST_CS|CONST_PERSISTENT);
	/* (250)  Transaction signature.  */
	REGISTER_LONG_CONSTANT("ARES_T_TSIG", ns_t_tsig, CONST_CS|CONST_PERSISTENT);
	/* (251)  Incremental zone transfer.  */
	REGISTER_LONG_CONSTANT("ARES_T_IXFR", ns_t_ixfr, CONST_CS|CONST_PERSISTENT);
	/* (252)  Transfer zone of authority.  */
	REGISTER_LONG_CONSTANT("ARES_T_AXFR", ns_t_axfr, CONST_CS|CONST_PERSISTENT);
	/* (253)  Transfer mailbox records.  */
	REGISTER_LONG_CONSTANT("ARES_T_MAILB", ns_t_mailb, CONST_CS|CONST_PERSISTENT);
	/* (254)  Transfer mail agent records.  */
	REGISTER_LONG_CONSTANT("ARES_T_MAILA", ns_t_maila, CONST_CS|CONST_PERSISTENT);
	/* (255)  Wildcard match.  */
	REGISTER_LONG_CONSTANT("ARES_T_ANY", ns_t_any, CONST_CS|CONST_PERSISTENT);
	
	/*
	 * ns_c (dnsclass) constants (arpa/nameser.h)
	 */
	
	/* (1)  Internet.  */
	REGISTER_LONG_CONSTANT("ARES_C_IN", ns_c_in, CONST_CS|CONST_PERSISTENT);
	/* (2)  unallocated/unsupported.  */
	/* REGISTER_LONG_CONSTANT("ARES_C_2", ns_c_2, CONST_CS|CONST_PERSISTENT); */
	/* (3)  MIT Chaos-net.  */
	REGISTER_LONG_CONSTANT("ARES_C_CHAOS", ns_c_chaos, CONST_CS|CONST_PERSISTENT);
	/* (4)  MIT Hesiod.  */
	REGISTER_LONG_CONSTANT("ARES_C_HS", ns_c_hs, CONST_CS|CONST_PERSISTENT);
	/* (254)  for prereq. sections in update requests  */
	/* REGISTER_LONG_CONSTANT("ARES_C_NONE", ns_c_none, CONST_CS|CONST_PERSISTENT); */
	/* (255)  Wildcard match.  */
	REGISTER_LONG_CONSTANT("ARES_C_ANY", ns_c_any, CONST_CS|CONST_PERSISTENT);
	
	le_ares = zend_register_list_destructors_ex(php_ares_le_dtor, NULL, PHP_ARES_LE_NAME, module_number);
	le_ares_query = zend_register_list_destructors_ex(php_ares_query_le_dtor, NULL, PHP_ARES_QUERY_LE_NAME, module_number);
	
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
static PHP_MINFO_FUNCTION(ares)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "AsyncResolver support", "enabled");
	php_info_print_table_row(2, "Version", PHP_ARES_VERSION);
	php_info_print_table_end();
	
	php_info_print_table_start();
	php_info_print_table_header(3, "Used Library", "compiled", "linked");
	php_info_print_table_row(3,
		PHP_ARES_LIBNAME,
#ifdef ARES_VERSION_STR
		ARES_VERSION_STR,
#else
		"unkown",
#endif
#ifdef HAVE_ARES_VERSION
		ares_version(NULL)
#else
		"unkown"
#endif
	);
	php_info_print_table_end();
}
/* }}} */

#ifdef ZEND_ENGINE_2
ZEND_BEGIN_ARG_INFO(ai_ares_select, 0)
		ZEND_ARG_PASS_INFO(1)
		ZEND_ARG_PASS_INFO(1)
		ZEND_ARG_PASS_INFO(0)
ZEND_END_ARG_INFO();

ZEND_BEGIN_ARG_INFO(ai_ares_result, 0)
		ZEND_ARG_PASS_INFO(0)
		ZEND_ARG_PASS_INFO(1)
		ZEND_ARG_PASS_INFO(1)
ZEND_END_ARG_INFO();

ZEND_BEGIN_ARG_INFO(ai_ares_fds, 0)
		ZEND_ARG_PASS_INFO(0)
		ZEND_ARG_PASS_INFO(1)
		ZEND_ARG_PASS_INFO(1)
ZEND_END_ARG_INFO();
#else
static unsigned char ai_ares_select[] = {3, BYREF_FORCE, BYREF_FORCE, BYREF_NONE};
static unsigned char ai_ares_result[] = {4, BYREF_NONE, BYREF_FORCE, BYREF_FORCE};
static unsigned char ai_ares_fds[] = {4, BYREF_NONE, BYREF_FORCE, BYREF_FORCE};
#endif

/* {{{ ares_functions[] */
zend_function_entry ares_functions[] = {
#ifdef HAVE_ARES_VERSION
	PHP_FE(ares_version, NULL)
#endif
	PHP_FE(ares_init, NULL)
	PHP_FE(ares_destroy, NULL)
	PHP_FE(ares_strerror, NULL)
#ifdef HAVE_ARES_CANCEL
	PHP_FE(ares_cancel, NULL)
#endif
	PHP_FE(ares_search, NULL)
	PHP_FE(ares_query, NULL)
	PHP_FE(ares_send, NULL)
	PHP_FE(ares_mkquery, NULL)
	PHP_FE(ares_gethostbyname, NULL)
	PHP_FE(ares_gethostbyaddr, NULL)
#ifdef HAVE_ARES_GETNAMEINFO
	PHP_FE(ares_getnameinfo, NULL)
#endif
	PHP_FE(ares_result, ai_ares_result)
	PHP_FE(ares_packet, NULL)
	PHP_FE(ares_process_all, NULL)
	PHP_FE(ares_process_once, NULL)
	PHP_FE(ares_process, NULL)
	PHP_FE(ares_select, ai_ares_select)
	PHP_FE(ares_fds, ai_ares_fds)
	PHP_FE(ares_timeout, NULL)
	PHP_FE(ares_parse_a_reply, NULL)
#ifdef HAVE_ARES_PARSE_AAAA_REPLY
	PHP_FE(ares_parse_aaaa_reply, NULL)
#endif
	PHP_FE(ares_parse_ptr_reply, NULL)
	PHP_FE(ares_expand_name, NULL)
#ifdef HAVE_ARES_EXPAND_STRING
	PHP_FE(ares_expand_string, NULL)
#endif
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ ares_module_entry */
zend_module_entry ares_module_entry = {
	STANDARD_MODULE_HEADER,
	"ares",
	ares_functions,
	PHP_MINIT(ares),
	NULL,
	NULL,
	NULL,
	PHP_MINFO(ares),
	PHP_ARES_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_ARES
ZEND_GET_MODULE(ares)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
