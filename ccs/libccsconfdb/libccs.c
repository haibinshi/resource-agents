/******************************************************************************
*******************************************************************************
**
**  Copyright (C) 2004-2008 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <openais/saAis.h>
#include <openais/confdb.h>

#include "ccs.h"

/* Callbacks are not supported yet */
static confdb_callbacks_t callbacks = {
	.confdb_change_notify_fn = NULL,
};

static confdb_handle_t handle = 0;

static char current_pos[PATH_MAX];
static char current_query[PATH_MAX];
static char previous_query[PATH_MAX];
static unsigned int query_handle;
static unsigned int list_handle;

/**
 * ccs_connect
 *
 * This function will only allow a connection if the node is part of
 * a quorate cluster.
 *
 * Returns: ccs_desc on success, < 0 on failure
 */
int ccs_connect(void){
	int res;

	memset(current_pos, 0, PATH_MAX);
	memset(current_query, 0, PATH_MAX);
	memset(previous_query, 0, PATH_MAX);
	query_handle = OBJECT_PARENT_HANDLE;
	list_handle = OBJECT_PARENT_HANDLE;

	res = confdb_initialize (&handle, &callbacks);
	if (res != SA_AIS_OK)
		return -1;

	return 1;
}

/**
 * ccs_force_connect
 *
 * This function will only allow a connection even if the node is not
 * part of a quorate cluster.  It will use the configuration file
 * as specified at build time (default: /etc/cluster/cluster.conf).  If that
 * file does not exist, a copy of the file will be broadcasted for.  If
 * blocking is specified, the broadcasts will be retried until a config file
 * is located.  Otherwise, the fuction will return an error if the initial
 * broadcast is not successful.
 *
 * Returns: ccs_desc on success, < 0 on failure
 */
int ccs_force_connect(const char *cluster_name, int blocking){
	int res = -1;

	if (blocking) {
		while ( res < 0 ) {
			res = ccs_connect();
			if (res < 0)
				sleep(1);
		}
		return res;
	} else
		return ccs_connect();
}

/**
 * ccs_disconnect
 * @desc: the descriptor returned by ccs_connect
 *
 * This function frees all associated state kept with an open connection
 *
 * Returns: 0 on success, < 0 on error
 */
int ccs_disconnect(int desc){
	int res;

	if (!handle)
		return 0;

	res = confdb_finalize (handle);
	if (res != CONFDB_OK)
		return -1;

	handle = 0;
	return 0;
}

/*
 * return 0 on success
 * return -1 on errors
 */
int path_dive()
{
	char *pos=NULL;
	unsigned int new_obj_handle;

	// for now we only handle absolute path queries
	if (strncmp(current_query, "/", 1))
		goto fail;

	pos = current_query + 1;

	while (pos)
	{
		char *next;

		/*
		 * if we still have "/" in the query we are still diving in the path
		 *
		 * XXX: what about /cluster/foo[@bar="/crap/whatever"]/@baz kind of queries?
		 * we _need_ sanity checks here
		 */

		next = strstr(pos, "/");

		if(!next) {
			pos = 0;
			continue;
		}

		memset(next, 0, 1);

		if(confdb_object_find_start(handle, query_handle) != SA_AIS_OK)
			goto fail;

		if (!strstr(pos, "[")) { /* straight path diving */
			if (confdb_object_find(handle, query_handle, pos, strlen(pos), &new_obj_handle) != SA_AIS_OK)
				goto fail;
			else
				query_handle = new_obj_handle;
		} else {
			/*
			 * /something[int]/ or /something[@foo="bar"]/
			 * start and end will identify []
			 * middle will point to the inside request
			 */

			char *start = NULL, *middle = NULL, *end = NULL;

			// we need a bit of sanity check to make sure we did parse everything correctly
			// for example end should always be > start...

			start=strstr(pos, "[");
			end=strstr(pos, "]");
			middle=start+1;
			memset(start, 0, 1);
			memset(end, 0, 1);

			if (!strstr(middle, "@")) { /* lookup something with index num = int */
				int val, i;

				val = atoi(middle);

				for (i = 1; i <= val; i++) {
					if (confdb_object_find(handle, query_handle, pos, strlen(pos), &new_obj_handle) != SA_AIS_OK)
						goto fail;
				}
				query_handle = new_obj_handle;

			} else { /* lookup something with obj foo = bar */
				char *equal = NULL, *value = NULL;
				char data[PATH_MAX];
				int goout = 0, datalen;

				memset(data, 0, PATH_MAX);

				// we need sanity checks here too!
				equal=strstr(middle, "=");
				memset(equal, 0, 1);

				value=strstr(equal + 1, "\"") + 1;
				memset(strstr(value, "\""), 0, 1);

				middle=strstr(middle, "@") + 1;

				// middle points to foo
				// value to bar

				while(!goout) {
					if (confdb_object_find(handle, query_handle, pos, strlen(pos), &new_obj_handle) != SA_AIS_OK)
						goto fail;
					else {
						if(confdb_key_get(handle, new_obj_handle, middle, strlen(middle), data, &datalen) == SA_AIS_OK) {
							if (!strcmp(data, value))
								goout=1;
						}
					}
				}
				query_handle=new_obj_handle;
			}
		}

		/* magic magic */
		pos = next + 1;
		memset(current_pos, 0, PATH_MAX);
		strcpy(current_pos, pos);
	}

	return 0;

fail:
	return -1;
}

int get_data(char **rtn, int list, int is_oldlist)
{
	int datalen, cmp;
	char data[PATH_MAX];
	char resval[PATH_MAX];
	char keyval[PATH_MAX];
	int keyvallen=PATH_MAX;
	unsigned int new_obj_handle;

	memset(data, 0, PATH_MAX);
	memset(resval, 0, PATH_MAX);
	memset(keyval, 0, PATH_MAX);

	// we need to handle child::*[int value] in non list mode.
	cmp = strcmp(current_pos, "child::*");
	if (cmp >= 0) {
		char *start = NULL, *end=NULL;
		int value = 1;

		// a pure child::* request should come down as list
		if (!cmp && !list)
			goto fail;

		if (!is_oldlist || cmp) {
			if(confdb_object_iter_start(handle, query_handle) != SA_AIS_OK)
				goto fail;

			list_handle = query_handle;
		}

		if(cmp) {
			// usual sanity checks here
			start=strstr(current_pos, "[") + 1;
			end=strstr(start, "]");
			memset(end, 0, 1);
			value=atoi(start);
			if (value <= 0)
				goto fail;
		}

		while (value) {
			if(confdb_object_iter(handle, query_handle, &new_obj_handle, data, &datalen) != SA_AIS_OK)
				goto fail;

			value--;
		}

		snprintf(resval, sizeof(resval), "%s=%s", data, keyval);
		*rtn = strndup(resval, datalen + keyvallen + 2);

	} else if (!strncmp(current_pos, "@*", strlen("@*"))) {

		// this query makes sense only if we are in list mode
		if(!list)
			goto fail;

		if (!is_oldlist)
			if(confdb_key_iter_start(handle, query_handle) != SA_AIS_OK)
				goto fail;

		list_handle = query_handle;

		if(confdb_key_iter(handle, query_handle, data, &datalen, keyval, &keyvallen) != SA_AIS_OK)
			goto fail;

		snprintf(resval, sizeof(resval), "%s=%s", data, keyval);
		*rtn = strndup(resval, datalen+keyvallen+2);

	} else { /* pure data request */
		char *query;

		// this query doesn't make sense in list mode
		if(list)
			goto fail;

		if(confdb_object_find_start(handle, query_handle) != SA_AIS_OK)
			goto fail;

		query = strstr(current_pos, "@") + 1;

		if(confdb_key_get(handle, query_handle, query, strlen(query), data, &datalen) != SA_AIS_OK)
			goto fail;

		*rtn = strndup(data, datalen);
	}

	return 0;

fail:
	return -1;
}

/**
 * _ccs_get
 * @desc:
 * @query:
 * @rtn: value returned
 * @list: 1 to operate in list fashion
 *
 * This function will allocate space for the value that is the result
 * of the given query.  It is the user's responsibility to ensure that
 * the data returned is freed.
 *
 * Returns: 0 on success, < 0 on failure
 */
int _ccs_get(int desc, const char *query, char **rtn, int list)
{
	int res = 0, confdbres = 0, is_oldlist = 0;

	/* we should be able to mangle the world here without destroying anything */
	strncpy(current_query, query, PATH_MAX-1);

	/* we need to check list mode */
	if (list && !strcmp(current_query, previous_query)) {
		query_handle = list_handle;
		is_oldlist = 1;
	} else {
		query_handle = OBJECT_PARENT_HANDLE;
		memset(previous_query, 0, PATH_MAX);
	}

	confdbres = confdb_object_find_start(handle, query_handle);
	if (confdbres != SA_AIS_OK) {
		res = -1;
		goto fail;
	}

	if(!is_oldlist) {
		res = path_dive(); /* remember path_dive cripples current_query */
		if (res < 0)
			goto fail;
	}

	strncpy(current_query, query, PATH_MAX-1); /* restore current_query */

	res = get_data(rtn, list, is_oldlist);
	if (res < 0)
		goto fail;

	if(list)
		strncpy(previous_query, query, PATH_MAX-1);

fail:
	return res;
}

int ccs_get(int desc, const char *query, char **rtn){
	return _ccs_get(desc, query, rtn, 0);
}

int ccs_get_list(int desc, const char *query, char **rtn){
	return _ccs_get(desc, query, rtn, 1);
}


/**
 * ccs_set: set an individual element's value in the config file.
 * @desc:
 * @path:
 * @val:
 *
 * This function is used to update individual elements in a config file.
 * It's effects are cluster wide.  It only succeeds when the node is part
 * of a quorate cluster.
 *
 * Note currently implemented.
 * 
 * Returns: 0 on success, < 0 on failure
 */
int ccs_set(int desc, const char *path, char *val){
	return -ENOSYS;
}

/**
 * ccs_lookup_nodename
 * @cd: ccs descriptor
 * @nodename: node name string
 * @retval: pointer to location to assign the result, if found
 *
 * This function takes any valid representation (FQDN, non-qualified
 * hostname, IP address, IPv6 address) of a node's name and finds its
 * canonical name (per cluster.conf). This function will find the primary
 * node name if passed a node's "altname" or any valid representation
 * of it.
 *
 * Returns: 0 on success, < 0 on failure
 */
int ccs_lookup_nodename(int cd, const char *nodename, char **retval) {
	char path[256];
	char host_only[128];
	char *str;
	char *p;
	int error;
	int ret;
	unsigned int i;
	size_t nodename_len;
	struct addrinfo hints;

	if (nodename == NULL)
		return (-1);

	nodename_len = strlen(nodename);
	ret = snprintf(path, sizeof(path),
			"/cluster/clusternodes/clusternode[@name=\"%s\"]/@name", nodename);
	if (ret < 0 || (size_t) ret >= sizeof(path))
		return (-E2BIG);

	str = NULL;
	error = ccs_get(cd, path, &str);
	if (!error) {
		*retval = str;
		return (0);
	}

	if (nodename_len >= sizeof(host_only))
		return (-E2BIG);

	/* Try just the hostname */
	strcpy(host_only, nodename);
	p = strchr(host_only, '.');
	if (p != NULL) {
		*p = '\0';

		ret = snprintf(path, sizeof(path),
				"/cluster/clusternodes/clusternode[@name=\"%s\"]/@name",
				host_only);
		if (ret < 0 || (size_t) ret >= sizeof(path))
			return (-E2BIG);

		str = NULL;
		error = ccs_get(cd, path, &str);
		if (!error) {
			*retval = str;
			return (0);
		}
	}

	memset(&hints, 0, sizeof(hints));
	if (strchr(nodename, ':') != NULL)
		hints.ai_family = AF_INET6;
	else if (isdigit(nodename[nodename_len - 1]))
		hints.ai_family = AF_INET;
	else
		hints.ai_family = AF_UNSPEC;

	/*
	** Try to match against each clusternode in cluster.conf.
	*/
	for (i = 1 ; ; i++) {
		char canonical_name[128];
		unsigned int altcnt;

		ret = snprintf(path, sizeof(path),
				"/cluster/clusternodes/clusternode[%u]/@name", i);
		if (ret < 0 || (size_t) ret >= sizeof(path))
			continue;

		for (altcnt = 0 ; ; altcnt++) {
			size_t len;
			struct addrinfo *ai = NULL;
			char cur_node[128];

			if (altcnt != 0) {
				ret = snprintf(path, sizeof(path), 
					"/cluster/clusternodes/clusternode[%u]/altname[%u]/@name",
					i, altcnt);
				if (ret < 0 || (size_t) ret >= sizeof(path))
					continue;
			}

			str = NULL;
			error = ccs_get(cd, path, &str);
			if (error || !str) {
				if (altcnt == 0)
					goto out_fail;
				break;
			}

			if (altcnt == 0) {
				if (strlen(str) >= sizeof(canonical_name)) {
					free(str);
					return (-E2BIG);
				}
				strcpy(canonical_name, str);
			}

			if (strlen(str) >= sizeof(cur_node)) {
				free(str);
				return (-E2BIG);
			}

			strcpy(cur_node, str);

			p = strchr(cur_node, '.');
			if (p != NULL)
				len = p - cur_node;
			else
				len = strlen(cur_node);

			if (strlen(host_only) == len &&
				!strncasecmp(host_only, cur_node, len))
			{
				free(str);
				*retval = strdup(canonical_name);
				if (*retval == NULL)
					return (-ENOMEM);
				return (0);
			}

			if (getaddrinfo(str, NULL, &hints, &ai) == 0) {
				struct addrinfo *cur;

				for (cur = ai ; cur != NULL ; cur = cur->ai_next) {
					char hostbuf[512];
					if (getnameinfo(cur->ai_addr, cur->ai_addrlen,
							hostbuf, sizeof(hostbuf),
							NULL, 0,
							hints.ai_family != AF_UNSPEC ? NI_NUMERICHOST : 0))
					{
						continue;
					}

					if (!strcasecmp(hostbuf, nodename)) {
						freeaddrinfo(ai);
						free(str);
						*retval = strdup(canonical_name);
						if (*retval == NULL)
							return (-ENOMEM);
						return (0);
					}
				}
				freeaddrinfo(ai);
			}

			free(str);

			/* Now try any altnames */
		}
	}

out_fail:
	*retval = NULL;
	return (-1);
}
