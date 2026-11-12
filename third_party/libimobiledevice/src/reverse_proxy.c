/*
 * reverse_proxy.c
 * com.apple.PurpleReverseProxy service implementation.
 *
 * Copyright (c) 2021 Nikias Bassen, All Rights Reserved.
 * Copyright (c) 2014 BALATON Zoltan. All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <string.h>
#include <stdlib.h>
#define _GNU_SOURCE 1
#define __USE_GNU 1
#include <stdio.h>
#include <errno.h>

#include <plist/plist.h>
#include <libimobiledevice-glue/thread.h>
#include <libimobiledevice-glue/socket.h>

#include "reverse_proxy.h"
#include "hv_life.h"
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "lockdown.h"
#include "common/debug.h"
#include "endianness.h"
#include "asprintf.h"

#ifndef ECONNRESET
#define ECONNRESET 108
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 138
#endif

#define CTRL_PORT 1082
#define CTRLCMD  "BeginCtrl"
#define HELLOCTRLCMD "HelloCtrl"
#define HELLOCMD "HelloConn"

#define RP_SYNC_MSG  0x1
#define RP_PROXY_MSG 0x105
#define RP_PLIST_MSG 0xbbaa

/**
 * Convert a service_error_t value to a reverse_proxy_error_t value.
 * Used internally to get correct error codes.
 *
 * @param err A service_error_t error code
 *
 * @return A matching reverse_proxy_error_t error code,
 *     REVERSE_PROXY_E_UNKNOWN_ERROR otherwise.
 */
static reverse_proxy_error_t reverse_proxy_error(service_error_t err)
{
	switch (err) {
		case SERVICE_E_SUCCESS:
			return REVERSE_PROXY_E_SUCCESS;
		case SERVICE_E_INVALID_ARG:
			return REVERSE_PROXY_E_INVALID_ARG;
		case SERVICE_E_MUX_ERROR:
			return REVERSE_PROXY_E_MUX_ERROR;
		case SERVICE_E_SSL_ERROR:
			return REVERSE_PROXY_E_SSL_ERROR;
		case SERVICE_E_NOT_ENOUGH_DATA:
			return REVERSE_PROXY_E_NOT_ENOUGH_DATA;
		case SERVICE_E_TIMEOUT:
			return REVERSE_PROXY_E_TIMEOUT;
		default:
			break;
	}
	return REVERSE_PROXY_E_UNKNOWN_ERROR;
}

static void _reverse_proxy_log(reverse_proxy_client_t client, const char* format, ...)
{
	if (!client || !client->log_cb) {
		return;
	}
	va_list args;
	va_start(args, format);
	char* buffer = NULL;
	if(vasprintf(&buffer, format, args)<0){}
	va_end(args);
	client->log_cb(client, buffer, client->log_cb_user_data);
	free(buffer);
}

static void _reverse_proxy_data(reverse_proxy_client_t client, int direction, char* buffer, uint32_t length)
{
	if (!client || !client->data_cb) {
		return;
	}
	client->data_cb(client, direction, buffer, length, client->data_cb_user_data);
}

static void _reverse_proxy_status(reverse_proxy_client_t client, int status, const char* format, ...)
{
	if (!client || !client->status_cb) {
		return;
	}
	va_list args;
	va_start(args, format);
	char* buffer = NULL;
	if(vasprintf(&buffer, format, args)<0){}
	va_end(args);
	client->status_cb(client, status, buffer, client->status_cb_user_data);
	free(buffer);
}


/* ---- Henge (I6): thread and socket lifetime ------------------------------------------------------------------------------------------------------------ */
struct rp_conn_node {
	THREAD_T th;
	uint64_t gen;
	int registered;       /* hv_life_task_begin succeeded: the thread's last act is hv_life_task_end */
	reverse_proxy_client_t client;   /* the control client the thread belongs to */
	struct rp_conn_node *next;
};

static reverse_proxy_client_t rp_root(reverse_proxy_client_t c) { return (c && c->root) ? c->root : c; }

/* 1 = this proxy must wind down: client_free asked for it, or the host session was cancelled/closed */
static int rp_stopped(reverse_proxy_client_t c)
{
	reverse_proxy_client_t r = rp_root(c);
	return (r && __atomic_load_n(&r->stop, __ATOMIC_SEQ_CST)) || hv_life_terminate_requested();
}

/* close an external socket that was registered with the host (contract rule 1): only when every reference came back */
static void rp_sock_close(uint64_t gen, int fd)
{
	if (fd < 0) return;
	if (hv_life_conn_remove(gen, fd) == 0) {
		socket_close(fd);
	}
	/* -1: a cancel still holds a reference (or the table refused): the fd is left open on purpose (a leak is safer than closing a number another thread may still shutdown()) */
}

/* Connect to an external host so that a cancel can reach it: the socket is registered (hv_life_conn_add) BEFORE connect() starts, so the host's shutdown() wakes the
   wait; the wait itself is sliced (100 ms) and re-checks rp_stopped(), so it also ends when the proxy is freed. The 5000 ms limit and the errno contract (-1 + errno)
   are those of glue's socket_connect(), whose policy hook (limd_glue_connect_guard) is called the same way. Name resolution (getaddrinfo) is not cancellable. */
static int rp_connect_cancelable(reverse_proxy_client_t client, const char *host, uint16_t port, uint64_t *gen_out)
{
	struct addrinfo hints, *result = NULL, *ai;
	char portstr[8];
	int sfd = -1, yes = 1, bufsize = 0x20000;
	uint64_t gen = 0;
	*gen_out = 0;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_protocol = IPPROTO_TCP;
	snprintf(portstr, sizeof(portstr), "%d", port);
	if (rp_stopped(client)) { errno = ECANCELED; return -1; }
	if (getaddrinfo(host, portstr, &hints, &result) != 0) { errno = EHOSTUNREACH; return -1; }
	int err = ECONNREFUSED;
	for (ai = result; ai; ai = ai->ai_next) {
		if (rp_stopped(client)) { err = ECANCELED; break; }
		sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (sfd < 0) continue;
		if (hv_life_conn_add(sfd, &gen) != 0) { close(sfd); sfd = -1; err = ECANCELED; break; }   /* refused: never registered, so a plain close is right */
		setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void*)&yes, sizeof(int));
		fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL, 0) | O_NONBLOCK);
		if (limd_glue_connect_guard && limd_glue_connect_guard(ai->ai_addr, ai->ai_addrlen) != 0) { rp_sock_close(gen, sfd); sfd = -1; err = EACCES; continue; }
		int ok = 0;
		if (connect(sfd, ai->ai_addr, ai->ai_addrlen) == 0) {
			ok = 1;
		} else if (errno == EINPROGRESS) {
			int waited = 0;
			while (waited < 5000) {
				struct pollfd pfd = { sfd, POLLOUT, 0 };
				int pr = poll(&pfd, 1, 100);
				if (pr > 0) {
					int so_error = 0; socklen_t l = sizeof(so_error);
					getsockopt(sfd, SOL_SOCKET, SO_ERROR, (void*)&so_error, &l);
					if (so_error == 0) ok = 1; else err = so_error;
					break;
				}
				if (pr < 0 && errno != EINTR) { err = errno; break; }
				if (rp_stopped(client)) { err = ECANCELED; break; }
				waited += 100;
			}
			if (!ok && waited >= 5000) err = ETIMEDOUT;
		} else {
			err = errno;
		}
		if (ok && rp_stopped(client)) { ok = 0; err = ECANCELED; }   /* connected, but we were told to stop: do not hand it out */
		if (ok) break;
		rp_sock_close(gen, sfd);
		sfd = -1;
		if (err == ECANCELED) break;
	}
	freeaddrinfo(result);
	if (sfd < 0) { errno = err; return -1; }
	setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void*)&yes, sizeof(int));
	setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void*)&bufsize, sizeof(int));
	setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, (void*)&bufsize, sizeof(int));
	*gen_out = gen;
	return sfd;
}

static int _reverse_proxy_handle_proxy_cmd(reverse_proxy_client_t client)
{
	reverse_proxy_error_t err = REVERSE_PROXY_E_SUCCESS;
	char *buf = NULL;
	size_t bufsize = 1048576;
	uint32_t sent = 0, bytes = 0;
	uint32_t sent_total = 0;
	uint32_t recv_total = 0;
	char *host = NULL;
	uint16_t port = 0;

	buf = malloc(bufsize);
	if (!buf) {
		_reverse_proxy_log(client, "ERROR: Failed to allocate buffer");
		return -1;
	}

	err = reverse_proxy_receive(client, buf, bufsize, &bytes);
	if (err != REVERSE_PROXY_E_SUCCESS) {
		free(buf);
		_reverse_proxy_log(client, "ERROR: Unable to read data for proxy command");
		return -1;
	}
	_reverse_proxy_log(client, "Handling proxy command");

	/* Just return success here unconditionally because we don't know
	 * anything else and we will eventually abort on failure anyway */
	uint16_t ack = 5;
	err = reverse_proxy_send(client, (char *)&ack, sizeof(ack), &sent);
	if (err != REVERSE_PROXY_E_SUCCESS || sent != sizeof(ack)) {
		free(buf);
		_reverse_proxy_log(client, "ERROR: Unable to send ack. Sent %u of %u bytes.", sent, (uint32_t)sizeof(ack));
		return -1;
	}

	if (bytes < 3) {
		free(buf);
		_reverse_proxy_log(client, "Proxy command data too short, retrying");
		return 0;
	}

	/* ack command data too */
	err = reverse_proxy_send(client, buf, bytes, &sent);
	if (err != REVERSE_PROXY_E_SUCCESS || sent != bytes) {
		free(buf);
		_reverse_proxy_log(client, "ERROR: Unable to send data. Sent %u of %u bytes.", sent, bytes);
		return -1;
	}

	/* Now try to handle actual messages */
	/* Connect: 0 3 hostlen <host> <port> */
	if (buf[0] == 0 && buf[1] == 3) {
		uint16_t *p = (uint16_t *)&buf[bytes - 2];
		port = be16toh(*p);
		buf[bytes - 2] = '\0';
		host = strdup(&buf[3]);
		_reverse_proxy_log(client, "Connect request to %s:%u", host, port);
	}

	if (!host || !buf[2]) {
		/* missing or zero length host name */
		free(host);
		free(buf);
		return 0;
	}

	/* else wait for messages and forward them */
	uint64_t sock_gen = 0;
	int sockfd = rp_connect_cancelable(client, host, port, &sock_gen);
	if (sockfd < 0) {
		free(buf);
		_reverse_proxy_log(client, "ERROR: Connection to %s:%u failed: %s", host, port, strerror(errno));
		free(host);
		return -1;
	}

	_reverse_proxy_status(client, RP_STATUS_CONNECTED, "Connected to %s:%u", host, port);

	int res = 0, bytes_ret;
	while (!rp_stopped(client)) {
		bytes = 0;
		err = reverse_proxy_receive_with_timeout(client, buf, bufsize, &bytes, 100);
		if (err == REVERSE_PROXY_E_TIMEOUT || (err == REVERSE_PROXY_E_SUCCESS && !bytes)) {
			/* just a timeout condition */
		}
		else if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(client, "Connection closed");
			res = -1;
			break;
		}
		if (bytes) {
			_reverse_proxy_log(client, "Proxying %u bytes of data", bytes);
			_reverse_proxy_data(client, RP_DATA_DIRECTION_OUT, buf, bytes);
			sent = 0;
			while (sent < bytes) {
				int s = socket_send(sockfd, buf + sent, bytes - sent);
				if (s < 0) {
					break;
				}
				sent += s;
			}
			sent_total += sent;
			if (sent != bytes) {
				_reverse_proxy_log(client, "ERROR: Sending proxy payload failed: %s. Sent %u of %u bytes.", strerror(errno), sent, bytes);
				res = -1;
				break;
			}
		}
		bytes_ret = socket_receive_timeout(sockfd, buf, bufsize, 0, 100);
		if (bytes_ret == -ETIMEDOUT) {
			bytes_ret = 0;
		} else if (bytes_ret == -ECONNRESET) {
			res = 1;
			break;
		} else if (bytes_ret < 0) {
			_reverse_proxy_log(client, "ERROR: Failed to receive from host: %s", strerror(-bytes_ret));
			break;
		}

		bytes = bytes_ret;
		if (bytes) {
			_reverse_proxy_log(client, "Received %u bytes reply data, sending to device\n", bytes);
			_reverse_proxy_data(client, RP_DATA_DIRECTION_IN, buf, bytes);
			recv_total += bytes;
			sent = 0;
			while (sent < bytes) {
				uint32_t s;
				err = reverse_proxy_send(client, buf + sent, bytes - sent, &s);
				if (err != REVERSE_PROXY_E_SUCCESS) {
					break;
				}
				sent += s;
			}
			if (err != REVERSE_PROXY_E_SUCCESS || bytes != sent) {
				_reverse_proxy_log(client, "ERROR: Unable to send data (%d). Sent %u of %u bytes.", err, sent, bytes);
				res = -1;
				break;
			}
		}
	}
	rp_sock_close(sock_gen, sockfd);   /* the one close (the send-failure branch used to close it and the line here closed it again) */
	free(host);
	free(buf);

	_reverse_proxy_status(client, RP_STATUS_DISCONNECTED, "Disconnected (out: %u / in: %u)", sent_total, recv_total);

	return res;
}

static int _reverse_proxy_handle_plist_cmd(reverse_proxy_client_t client)
{
	plist_t dict;
	reverse_proxy_error_t err;

	err = reverse_proxy_receive_plist(client, &dict);
	if (err != REVERSE_PROXY_E_SUCCESS) {
		_reverse_proxy_log(client, "ERROR: Unable to receive plist command, error", err);
		return -1;
	}
	plist_t node = plist_dict_get_item(dict, "Command");
	if (!node || (plist_get_node_type(node) != PLIST_STRING)) {
		_reverse_proxy_log(client, "ERROR: No 'Command' in reply", err);
		plist_free(dict);
		return -1;
	}
	char *command = NULL;
	plist_get_string_val(node, &command);
	plist_free(dict);

	if (!command) {
		_reverse_proxy_log(client, "ERROR: Empty 'Command' string");
		return -1;
	}

	if (!strcmp(command, "Ping")) {
		_reverse_proxy_log(client, "Received Ping command, replying with Pong");
		dict = plist_new_dict();
		plist_dict_set_item(dict, "Pong", plist_new_bool(1));
		err = reverse_proxy_send_plist(client, dict);
		plist_free(dict);
		if (err) {
			_reverse_proxy_log(client, "ERROR: Unable to send Ping command reply");
			free(command);
			return -1;
		}
	} else {
		_reverse_proxy_log(client, "WARNING: Received unhandled plist command '%s'", command);
		free(command);
		return -1;
	}

	free(command);
	/* reverse proxy connection will be terminated remotely. Next receive will get nothing, error and terminate this worker thread. */
	return 0;
}

static reverse_proxy_error_t reverse_proxy_client_new(idevice_t device, lockdownd_service_descriptor_t service, reverse_proxy_client_t * client)
{
	*client = NULL;

	if (!device || !service || service->port == 0 || !client || *client) {
		return REVERSE_PROXY_E_INVALID_ARG;
	}

	debug_info("Creating reverse_proxy_client, port = %d.", service->port);

	service_client_t sclient = NULL;
	reverse_proxy_error_t ret = reverse_proxy_error(service_client_new(device, service, &sclient));
	if (ret != REVERSE_PROXY_E_SUCCESS) {
		debug_info("Creating service client failed. Error: %i", ret);
		return ret;
	}

	reverse_proxy_client_t client_loc = (reverse_proxy_client_t) calloc(1, sizeof(struct reverse_proxy_client_private));
	client_loc->parent = sclient;
	client_loc->th_ctrl = THREAD_T_NULL;
	*client = client_loc;

	return 0;
}

static void* _reverse_proxy_connection_thread(void *cdata)
{
	struct rp_conn_node *node = (struct rp_conn_node *)cdata;
	reverse_proxy_client_t client = node->client;
	uint32_t bytes = 0;
	reverse_proxy_client_t conn_client = NULL;
	reverse_proxy_error_t err = REVERSE_PROXY_E_UNKNOWN_ERROR;

	if (client->conn_port == 0) {
		service_client_factory_start_service(client->parent->connection->device, "com.apple.PurpleReverseProxy.Conn", (void**)&conn_client, client->label, SERVICE_CONSTRUCTOR(reverse_proxy_client_new), &err);
		if (!conn_client) {
			_reverse_proxy_log(client, "ERROR: Failed to start proxy connection service, error %d", err);
		}
	} else {
		struct lockdownd_service_descriptor svc;
		svc.port = client->conn_port;
		svc.ssl_enabled = 0;
		svc.identifier = NULL;
		err = reverse_proxy_client_new(client->parent->connection->device, &svc, &conn_client);
		if (!conn_client) {
			_reverse_proxy_log(client, "ERROR: Failed to connect to proxy connection port %u, error %d", client->conn_port, err);
		}
	}
	if (!conn_client) {
		goto leave;
	}
	conn_client->type = RP_TYPE_CONN;
	conn_client->root = client;
	conn_client->protoversion = client->protoversion;
	conn_client->log_cb = client->log_cb;
	conn_client->log_cb_user_data = client->log_cb_user_data;
	conn_client->status_cb = client->status_cb;
	conn_client->status_cb_user_data = client->status_cb_user_data;

	err = reverse_proxy_send(conn_client, HELLOCMD, sizeof(HELLOCMD), &bytes);
	if (err != REVERSE_PROXY_E_SUCCESS || bytes != sizeof(HELLOCMD)) {
		_reverse_proxy_log(conn_client, "ERROR: Unable to send " HELLOCMD " (sent %u/%u bytes)", bytes, sizeof(HELLOCMD));
		goto leave;
	}

	if (conn_client->protoversion == 2) {
		plist_t reply = NULL;
		err = reverse_proxy_receive_plist(conn_client, &reply);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(conn_client, "ERROR: Did not receive " HELLOCMD " reply, error %d", err);
			goto leave;
		}
		char* identifier = NULL;
		char* cmd = NULL;
		plist_t node = NULL;
		node = plist_dict_get_item(reply, "Command");
		if (node) {
			plist_get_string_val(node, &cmd);
		}
		node = plist_dict_get_item(reply, "Identifier");
		if (node) {
			plist_get_string_val(node, &identifier);
		}
		plist_free(reply);

		if (!cmd || (strcmp(cmd, HELLOCMD) != 0)) {
			free(cmd);
			free(identifier);
			_reverse_proxy_log(conn_client, "ERROR: Unexpected reply to " HELLOCMD " received");
			goto leave;
		}
		free(cmd);

		if (identifier) {
			_reverse_proxy_log(conn_client, "Got device identifier %s", identifier);
			free(identifier);
		}
	} else {
		char buf[16];
		memset(buf, '\0', sizeof(buf));
		bytes = 0;
		err = reverse_proxy_receive(conn_client, buf, sizeof(HELLOCMD), &bytes);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(conn_client, "ERROR: Did not receive " HELLOCMD " reply, error %d", err);
			goto leave;
		}
		if (memcmp(buf, HELLOCMD, sizeof(HELLOCMD)) != 0) {
			_reverse_proxy_log(conn_client, "ERROR: Did not receive " HELLOCMD " as reply, but %.*s", (int)bytes, buf);
			goto leave;
		}
	}

	_reverse_proxy_status(conn_client, RP_STATUS_READY, "Ready");

	int running = 1;
	while (!rp_stopped(client) && conn_client && running) {
		uint16_t cmd = 0;
		bytes = 0;
		err = reverse_proxy_receive_with_timeout(conn_client, (char*)&cmd, sizeof(cmd), &bytes, 1000);
		if (err == REVERSE_PROXY_E_TIMEOUT || (err == REVERSE_PROXY_E_SUCCESS && bytes != sizeof(cmd))) {
			continue;
		} else if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(conn_client, "Connection closed");
			break;
		}
		cmd = le16toh(cmd);
		switch (cmd) {
		case 0xBBAA:
			/* plist command */
			if (_reverse_proxy_handle_plist_cmd(conn_client) < 0) {
				running = 0;
			}
			break;
		case 0x105:
			/* proxy command */
			if (_reverse_proxy_handle_proxy_cmd(conn_client) < 0) {
				running = 0;
			}
			break;
		default:
			/* unknown */
			debug_info("ERROR: Unknown request 0x%x", cmd);
			_reverse_proxy_log(conn_client, "ERROR: Unknown request 0x%x", cmd);
			running = 0;
			break;
		}
	}

leave:
	_reverse_proxy_status(conn_client, RP_STATUS_TERMINATE, "Terminated");
	if (conn_client) {
		reverse_proxy_client_free(conn_client);
	}

	/* the thread's last act: nothing shared is touched after this (the node and the control client are released only after the join) */
	if (node->registered) {
		hv_life_task_end(node->gen, HV_LIFE_K_PROXY_CONN, 0);
	}
	return NULL;
}

static void* _reverse_proxy_control_thread(void *cdata)
{
	reverse_proxy_client_t client = (reverse_proxy_client_t)cdata;
	int running = 1;
	int join_failed = 0;
	_reverse_proxy_status(client, RP_STATUS_READY, "Ready");
	while (client && !rp_stopped(client) && running) {
		uint32_t cmd = 0;
		uint32_t bytes = 0;
		reverse_proxy_error_t err = reverse_proxy_receive_with_timeout(client, (char*)&cmd, sizeof(cmd), &bytes, 1000);
		if (err == REVERSE_PROXY_E_TIMEOUT || (err == REVERSE_PROXY_E_SUCCESS && bytes != sizeof(cmd))) {
			continue;
		} else if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(client, "Connection closed");
			break;
		}
		cmd = le32toh(cmd);
		switch (cmd) {
		case 1:
			/* connection request */
			debug_info("ReverseProxy<%p> got connect request", client);
			_reverse_proxy_status(client, RP_STATUS_CONNECT_REQ, "Connect Request");
			{
				/* every connection thread has its own node: the handle is kept until it is joined (the single 'th_conn' used to be overwritten, so only the last one was joined) */
				struct rp_conn_node *node = (struct rp_conn_node *)calloc(1, sizeof(*node));
				if (!node) { running = 0; break; }
				node->client = client;
				node->registered = (hv_life_task_begin(HV_LIFE_K_PROXY_CONN, &node->gen) == 0);
				if (!node->registered) {   /* refused by the host session (closing): no thread is created */
					_reverse_proxy_log(client, "Connection thread refused");
					free(node);
					running = 0;
					break;
				}
				if (thread_new(&node->th, _reverse_proxy_connection_thread, node) != 0) {
					debug_info("ERROR: Failed to start connection thread");
					hv_life_task_create_failed(node->gen, HV_LIFE_K_PROXY_CONN, 1);
					free(node);
					running = 0;
					break;
				}
				node->next = client->conns;
				client->conns = node;
			}
			break;
		case 2:
			/* shutdown request */
			debug_info("ReverseProxy<%p> got shutdown request", client);
			_reverse_proxy_status(client, RP_STATUS_SHUTDOWN_REQ, "Shutdown Request");
			running = 0;
			break;
		default:
			/* unknown */
			debug_info("ERROR: Unknown request 0x%x", cmd);
			_reverse_proxy_log(client, "ERROR: Unknown request 0x%x", cmd);
			running = 0;
			break;
		}
	}
	_reverse_proxy_log(client, "Terminating");

	/* tell the connection threads to leave, then join EVERY one of them. A node whose join fails is kept (leaked) rather than freed under a thread that may still run. */
	__atomic_store_n(&client->stop, 1, __ATOMIC_SEQ_CST);
	struct rp_conn_node *n = client->conns;
	client->conns = NULL;
	while (n) {
		struct rp_conn_node *next = n->next;
		debug_info("joining connection thread");
		if (thread_join(n->th) == 0) {
			thread_free(n->th);
			free(n);
		} else {
			join_failed = 1;
		}
		n = next;
	}

	_reverse_proxy_status(client, RP_STATUS_TERMINATE, "Terminated");

	/* last act of the control thread */
	hv_life_task_end(client->ctrl_gen, HV_LIFE_K_PROXY_CTRL, join_failed ? 1 : 0);
	return NULL;
}

reverse_proxy_error_t reverse_proxy_client_start_proxy(reverse_proxy_client_t client, int control_protocol_version)
{
	char buf[16] = {0, };
	uint32_t bytes = 0;
	reverse_proxy_error_t err = REVERSE_PROXY_E_UNKNOWN_ERROR;

	if (!client) {
		return REVERSE_PROXY_E_INVALID_ARG;
	}
	if (control_protocol_version < 1 || control_protocol_version > 2) {
		debug_info("invalid protocol version %d, must be 1 or 2", control_protocol_version);
		return REVERSE_PROXY_E_INVALID_ARG;
	}

	if (control_protocol_version == 2) {
		err = reverse_proxy_send(client, CTRLCMD, sizeof(CTRLCMD), &bytes);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(client, "ERROR: Failed to send " CTRLCMD " to device, error %d", err);
			return err;
		}
		plist_t dict = plist_new_dict();
		plist_dict_set_item(dict, "Command", plist_new_string(CTRLCMD));
		plist_dict_set_item(dict, "CtrlProtoVersion", plist_new_uint(client->protoversion));
		err = reverse_proxy_send_plist(client, dict);
		plist_free(dict);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(client, "ERROR: Could not send " CTRLCMD " plist command, error %d", err);
			return err;
		}
		dict = NULL;
		err = reverse_proxy_receive_plist(client, &dict);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(client, "ERROR: Could not receive " CTRLCMD " plist reply, error %d", err);
			return err;
		}
		plist_t node = plist_dict_get_item(dict, "ConnPort");
		if (node && plist_get_node_type(node) == PLIST_UINT) {
			uint64_t u64val = 0;
			plist_get_uint_val(node, &u64val);
			client->conn_port = (uint16_t)u64val;
		} else {
			_reverse_proxy_log(client, "ERROR: Could not get ConnPort value");
			return REVERSE_PROXY_E_UNKNOWN_ERROR;
		}
		client->protoversion = 2;
	} else {
		err = reverse_proxy_send(client, HELLOCTRLCMD, sizeof(HELLOCTRLCMD), &bytes);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(client, "ERROR: Failed to send " HELLOCTRLCMD " to device, error %d", err);
			return err;
		}

		bytes = 0;
		err = reverse_proxy_receive(client, buf, sizeof(HELLOCTRLCMD)-1, &bytes);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(client, "ERROR: Could not receive " HELLOCTRLCMD " reply, error %d", err);
			return err;
		}

		uint16_t cport = 0;
		bytes = 0;
		err = reverse_proxy_receive(client, (char*)&cport, 2, &bytes);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			_reverse_proxy_log(client, "ERROR: Failed to receive connection port, error %d", err);
			return err;
		}
		client->conn_port = le16toh(cport);
		client->protoversion = 1;
	}

	if (hv_life_task_begin(HV_LIFE_K_PROXY_CTRL, &client->ctrl_gen) != 0) {   /* refused by the host session: no thread */
		_reverse_proxy_log(client, "ERROR: Control thread refused");
		return REVERSE_PROXY_E_UNKNOWN_ERROR;
	}
	if (thread_new(&(client->th_ctrl), _reverse_proxy_control_thread, client) != 0) {
		_reverse_proxy_log(client, "ERROR: Failed to start control thread");
		hv_life_task_create_failed(client->ctrl_gen, HV_LIFE_K_PROXY_CTRL, 1);
		client->th_ctrl = THREAD_T_NULL; /* undefined after failure */
		err = REVERSE_PROXY_E_UNKNOWN_ERROR;
	}

	return err;
}

reverse_proxy_error_t reverse_proxy_client_create_with_service(idevice_t device, reverse_proxy_client_t* client, const char* label)
{
	reverse_proxy_error_t err = REVERSE_PROXY_E_UNKNOWN_ERROR;
	service_client_factory_start_service(device, "com.apple.PurpleReverseProxy.Ctrl", (void**)client, label, SERVICE_CONSTRUCTOR(reverse_proxy_client_new), &err);
	if (!*client) {
		return err;
	}
	(*client)->label = strdup(label);
	(*client)->type = RP_TYPE_CTRL;

	return REVERSE_PROXY_E_SUCCESS;
}

reverse_proxy_error_t reverse_proxy_client_create_with_port(idevice_t device, reverse_proxy_client_t* client, uint16_t device_port)
{
	reverse_proxy_client_t client_loc = NULL;
	reverse_proxy_error_t err;

	struct lockdownd_service_descriptor svc;
	svc.port = device_port;
	svc.ssl_enabled = 0;
	svc.identifier = NULL;

	err = reverse_proxy_client_new(device, &svc, &client_loc);
	if (err != REVERSE_PROXY_E_SUCCESS) {
		return err;
	}

	client_loc->type = RP_TYPE_CTRL;
	*client = client_loc;

	return REVERSE_PROXY_E_SUCCESS;
}

reverse_proxy_error_t reverse_proxy_client_free(reverse_proxy_client_t client)
{
	if (!client)
		return REVERSE_PROXY_E_INVALID_ARG;
	/* 1. ask every thread of this proxy to leave; 2. wait for the control thread (it joins all connection threads); 3. only then free the parent connection and the client.
	   If the join cannot be confirmed nothing is freed (a thread may still be using it). */
	__atomic_store_n(&client->stop, 1, __ATOMIC_SEQ_CST);
	if (client->th_ctrl) {
		debug_info("joining control thread");
		if (thread_join(client->th_ctrl) != 0) {
			return REVERSE_PROXY_E_UNKNOWN_ERROR;
		}
		thread_free(client->th_ctrl);
		client->th_ctrl = THREAD_T_NULL;
	}
	service_client_t parent = client->parent;
	client->parent = NULL;
	reverse_proxy_error_t err = reverse_proxy_error(service_client_free(parent));
	free(client->label);
	free(client);

	return err;
}

reverse_proxy_client_type_t reverse_proxy_get_type(reverse_proxy_client_t client)
{
	if (!client)
		return 0;
	return client->type;
}

void reverse_proxy_client_set_status_callback(reverse_proxy_client_t client, reverse_proxy_status_cb_t status_callback, void* user_data)
{
	if (!client) {
		return;
	}
	client->status_cb = status_callback;
	client->status_cb_user_data = user_data;
}

void reverse_proxy_client_set_log_callback(reverse_proxy_client_t client, reverse_proxy_log_cb_t log_callback, void* user_data)
{
	if (!client) {
		return;
	}
	client->log_cb = log_callback;
	client->log_cb_user_data = user_data;
}

void reverse_proxy_client_set_data_callback(reverse_proxy_client_t client, reverse_proxy_data_cb_t data_callback, void* user_data)
{
	if (!client) {
		return;
	}
	client->data_cb = data_callback;
	client->data_cb_user_data = user_data;
}

reverse_proxy_error_t reverse_proxy_send(reverse_proxy_client_t client, const char* data, uint32_t len, uint32_t* sent)
{
	reverse_proxy_error_t err = reverse_proxy_error(service_send(client->parent, data, len, sent));
	return err;
}

reverse_proxy_error_t reverse_proxy_receive_with_timeout(reverse_proxy_client_t client, char* buffer, uint32_t len, uint32_t* received, unsigned int timeout)
{
	if (!client)
		return REVERSE_PROXY_E_INVALID_ARG;
	return reverse_proxy_error(service_receive_with_timeout(client->parent, buffer, len, received, timeout));
}

reverse_proxy_error_t reverse_proxy_receive(reverse_proxy_client_t client, char* buffer, uint32_t len, uint32_t* received)
{
	return reverse_proxy_receive_with_timeout(client, buffer, len, received, 20000);
}

reverse_proxy_error_t reverse_proxy_send_plist(reverse_proxy_client_t client, plist_t plist)
{
	reverse_proxy_error_t err;
	uint32_t len = 0;
	char* buf = NULL;
	uint32_t bytes = 0;

	plist_to_bin(plist, &buf, &len);

	if (!buf) {
		return REVERSE_PROXY_E_INVALID_ARG;
	}

	debug_info("Sending %u bytes", len);

	uint32_t slen = htole32(len);
	err = reverse_proxy_send(client, (char*)&slen, sizeof(slen), &bytes);
	if (err != REVERSE_PROXY_E_SUCCESS) {
		free(buf);
		debug_info("ERROR: Unable to send data length, error %d. Sent %u/%u bytes.", err, bytes, (uint32_t)sizeof(slen));
		return err;
	}
	uint32_t done = 0;
	do {
		bytes = 0;
		err = reverse_proxy_send(client, buf+done, len-done, &bytes);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			break;
		}
		done += bytes;
	} while (done < len);
	free(buf);
	if (err != REVERSE_PROXY_E_SUCCESS || done != len) {
		debug_info("ERROR: Unable to send data, error %d. Sent %u/%u bytes.", err, done, len);
		return err;
	}

	debug_info("Sent %u bytes", len);

	return REVERSE_PROXY_E_SUCCESS;
}

reverse_proxy_error_t reverse_proxy_receive_plist(reverse_proxy_client_t client, plist_t* plist)
{
	return reverse_proxy_receive_plist_with_timeout(client, plist, 20000);
}

reverse_proxy_error_t reverse_proxy_receive_plist_with_timeout(reverse_proxy_client_t client, plist_t * plist, uint32_t timeout_ms)
{
	uint32_t len;
	uint32_t bytes;
	reverse_proxy_error_t err;

	err = reverse_proxy_receive_with_timeout(client, (char*)&len, sizeof(len), &bytes, timeout_ms);
	if (err != REVERSE_PROXY_E_SUCCESS) {
		if (err != REVERSE_PROXY_E_TIMEOUT) {
			debug_info("ERROR: Unable to receive packet length, error %d\n", err);
		}
		return err;
	}

	len = le32toh(len);
	char* buf = calloc(1, len);
	if (!buf) {
		debug_info("ERROR: Out of memory");
		return REVERSE_PROXY_E_UNKNOWN_ERROR;
	}

	uint32_t done = 0;
	do {
		bytes = 0;
		err = reverse_proxy_receive_with_timeout(client, buf+done, len-done, &bytes, timeout_ms);
		if (err != REVERSE_PROXY_E_SUCCESS) {
			break;
		}
		done += bytes;
	} while (done < len);

	if (err != REVERSE_PROXY_E_SUCCESS || done != len) {
		free(buf);
		debug_info("ERROR: Unable to receive data, error %d. Received %u/%u bytes.", err, done, len);
		return err;
	}

	debug_info("Received %u bytes", len);

	plist_from_bin(buf, len, plist);
	free(buf);

	if (!(*plist)) {
		debug_info("ERROR: Failed to convert buffer to plist");
		return REVERSE_PROXY_E_PLIST_ERROR;
	}

	return REVERSE_PROXY_E_SUCCESS;
}
