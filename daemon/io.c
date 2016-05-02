/*  Copyright (C) 2014 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <libknot/errcode.h>
#include <contrib/ucw/lib.h>
#include <contrib/ucw/mempool.h>

#include "daemon/io.h"
#include "daemon/network.h"
#include "daemon/worker.h"

#define negotiate_bufsize(func, handle, bufsize_want) do { \
    int bufsize = 0; func(handle, &bufsize); \
	if (bufsize < bufsize_want) { \
		bufsize = bufsize_want; \
		func(handle, &bufsize); \
	} \
} while (0)

static void check_bufsize(uv_handle_t* handle)
{
	/* We want to buffer at least N waves in advance.
	 * This is magic presuming we can pull in a whole recvmmsg width in one wave.
	 * Linux will double this the bufsize wanted.
	 */
	const int bufsize_want = RECVMMSG_BATCH * 65535 * 2;
	negotiate_bufsize(uv_recv_buffer_size, handle, bufsize_want);
	negotiate_bufsize(uv_send_buffer_size, handle, bufsize_want);
}

#undef negotiate_bufsize

static void *handle_alloc(uv_loop_t *loop, size_t size)
{
	return malloc(size);
}

static void handle_free(uv_handle_t *handle)
{
	free(handle);
}

static void handle_getbuf(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	/* Worker has single buffer which is reused for all incoming
	 * datagrams / stream reads, the content of the buffer is
	 * guaranteed to be unchanged only for the duration of
	 * udp_read() and tcp_read().
	 */
	uv_loop_t *loop = handle->loop;
	struct worker_ctx *worker = loop->data;
	buf->base = (char *)worker->wire_buf;
	/* Use recvmmsg() on master sockets if possible. */
	if (handle->data)
		buf->len = suggested_size;
	else
		buf->len = sizeof(worker->wire_buf);
}

void udp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
	const struct sockaddr *addr, unsigned flags)
{
	uv_loop_t *loop = handle->loop;
	struct worker_ctx *worker = loop->data;
	if (nread <= 0) {
		if (nread < 0) { /* Error response, notify resolver */
			worker_exec(worker, (uv_handle_t *)handle, NULL, addr);
		} /* nread == 0 is for freeing buffers, we don't need to do this */
		return;
	}

	knot_pkt_t *query = knot_pkt_new(buf->base, nread, &worker->pkt_pool);
	if (query) {
		query->max_size = KNOT_WIRE_MAX_PKTSIZE;
		worker_exec(worker, (uv_handle_t *)handle, query, addr);
	}
	mp_flush(worker->pkt_pool.ctx);
}

int udp_bind(uv_udp_t *handle, struct sockaddr *addr)
{
	unsigned flags = UV_UDP_REUSEADDR;
	if (addr->sa_family == AF_INET6) {
		flags |= UV_UDP_IPV6ONLY;
	}
	int ret = uv_udp_bind(handle, addr, flags);
	if (ret != 0) {
		return ret;
	}
	handle->data = NULL;
	check_bufsize((uv_handle_t *)handle);
	return io_start_read((uv_handle_t *)handle);
}

static void tcp_recv(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf)
{
	uv_loop_t *loop = handle->loop;
	struct worker_ctx *worker = loop->data;

	/* Check for originator connection close. */
	if (nread <= 0) {
		if (handle->data) {
			worker_exec(worker, (uv_handle_t *)handle, NULL, NULL);
		}
		if (!uv_is_closing((uv_handle_t *)handle)) {
			uv_close((uv_handle_t *)handle, handle_free);
		}
		return;
	}
	
	int ret = worker_process_tcp(worker, (uv_handle_t *)handle, (const uint8_t *)buf->base, nread);
	if (ret == 0) {
		/* Push - pull, stop reading from this handle until
		 * the task is finished. Since the handle has no track of the
		 * pending tasks, it might be freed before the task finishes
		 * leading various errors. */
		uv_unref((uv_handle_t *)handle);
		io_stop_read((uv_handle_t *)handle);
	}
	mp_flush(worker->pkt_pool.ctx);
}

static void tcp_accept(uv_stream_t *master, int status)
{
	if (status != 0) {
		return;
	}

	uv_stream_t *client = handle_alloc(master->loop, sizeof(*client));
	if (!client) {
		return;
	}
	memset(client, 0, sizeof(*client));
	io_create(master->loop, (uv_handle_t *)client, SOCK_STREAM);
	if (uv_accept(master, client) != 0) {
		handle_free((uv_handle_t *)client);
		return;
	}

	io_start_read((uv_handle_t *)client);
}

int tcp_bind(uv_tcp_t *handle, struct sockaddr *addr)
{
	unsigned flags = UV_UDP_REUSEADDR;
	if (addr->sa_family == AF_INET6) {
		flags |= UV_UDP_IPV6ONLY;
	}
	int ret = uv_tcp_bind(handle, addr, flags);
	if (ret != 0) {
		return ret;
	}

	ret = uv_listen((uv_stream_t *)handle, 16, tcp_accept);
	if (ret != 0) {
		return ret;
	}

	handle->data = NULL;
	return 0;
}

void io_create(uv_loop_t *loop, uv_handle_t *handle, int type)
{
	if (type == SOCK_DGRAM) {
		uv_udp_init(loop, (uv_udp_t *)handle);
	} else {
		uv_tcp_init(loop, (uv_tcp_t *)handle);
		uv_tcp_nodelay((uv_tcp_t *)handle, 1);
	}
}

int io_start_read(uv_handle_t *handle)
{
	if (handle->type == UV_UDP) {
		return uv_udp_recv_start((uv_udp_t *)handle, &handle_getbuf, &udp_recv);
	} else {
		return uv_read_start((uv_stream_t *)handle, &handle_getbuf, &tcp_recv);
	}
}

int io_stop_read(uv_handle_t *handle)
{
	if (handle->type == UV_UDP) {
		return uv_udp_recv_stop((uv_udp_t *)handle);
	} else {
		return uv_read_stop((uv_stream_t *)handle);
	}
}
