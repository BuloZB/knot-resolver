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

#include <uv.h>
#include <lua.h>
#include <libknot/packet/pkt.h>
#include <libknot/descriptor.h>
#include <contrib/ucw/lib.h>
#include <contrib/ucw/mempool.h>
#include <contrib/wire.h>
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
#include <malloc.h>
#endif
#include <assert.h>
#include "lib/utils.h"
#include "lib/layer.h"
#include "daemon/worker.h"
#include "daemon/engine.h"
#include "daemon/io.h"

/* @internal IO request entry. */
struct ioreq
{
	union {
		uv_udp_t      udp;
		uv_tcp_t      tcp;
		uv_udp_send_t send;
		uv_write_t    write;
		uv_connect_t  connect;
	} as;
};

/** @internal Number of request within timeout window. */
#define MAX_PENDING (KR_NSREP_MAXADDR + (KR_NSREP_MAXADDR / 2))

/** @internal Debugging facility. */
#ifdef DEBUG
#define DEBUG_MSG(fmt...) printf("[daem] " fmt)
#else
#define DEBUG_MSG(fmt...)
#endif

/** @internal Query resolution task. */
struct qr_task
{
	struct kr_request req;
	struct worker_ctx *worker;
	knot_pkt_t *pktbuf;
	array_t(struct qr_task *) waiting;
	uv_handle_t *pending[MAX_PENDING];
	uint16_t pending_count;
	uint16_t addrlist_count;
	uint16_t addrlist_turn;
	struct sockaddr *addrlist;
	uv_timer_t retry, timeout;
	worker_cb_t on_complete;
	void *baton;
	struct {
		union {
			struct sockaddr_in ip4;
			struct sockaddr_in6 ip6;
		} addr;
		uv_handle_t *handle;
	} source;
	uint16_t iter_count;
	uint16_t refs;
	uint16_t bytes_remaining;
	bool finished;
	bool leading;
};

/* Convenience macros */
#define qr_task_ref(task) \
	do { ++(task)->refs; } while(0)
#define qr_task_unref(task) \
	do { if (--(task)->refs == 0) { qr_task_free(task); } } while (0)
#define qr_valid_handle(task, checked) \
	(!uv_is_closing((checked)) || (task)->source.handle == (checked))

/* Forward decls */
static int qr_task_step(struct qr_task *task, const struct sockaddr *packet_source, knot_pkt_t *packet);

/** @internal Get singleton worker. */
static inline struct worker_ctx *get_worker(void)
{
	return uv_default_loop()->data;
}

static inline struct ioreq *ioreq_take(struct worker_ctx *worker)
{
	struct ioreq *req = NULL;
	if (worker->ioreqs.len > 0) {
		req = array_tail(worker->ioreqs);
		array_pop(worker->ioreqs);
	} else {
		req = malloc(sizeof(*req));
	}
	kr_asan_unpoison(req, sizeof(*req));
	return req;
}

static inline void ioreq_release(struct worker_ctx *worker, struct ioreq *req)
{
	kr_asan_poison(req, sizeof(*req));
	if (!req || worker->ioreqs.len < 4 * MP_FREELIST_SIZE) {
		array_push(worker->ioreqs, req);
	} else {
		free(req);
	}
}

static uv_handle_t *ioreq_spawn(struct qr_task *task, int socktype)
{
	if (task->pending_count >= MAX_PENDING) {
		return NULL;
	}
	/* Create connection for iterative query */
	uv_handle_t *req = (uv_handle_t *)ioreq_take(task->worker);
	if (!req) {
		return NULL;
	}
	io_create(task->worker->loop, req, socktype);
	req->data = task;
	/* Connect or issue query datagram */
	task->pending[task->pending_count] = req;
	task->pending_count += 1;
	return req;
}

static void ioreq_on_close(uv_handle_t *handle)
{
	struct worker_ctx *worker = get_worker();
	ioreq_release(worker, (struct ioreq *)handle);
}

static void ioreq_kill(uv_handle_t *req)
{
	assert(req);
	if (!uv_is_closing(req)) {
		io_stop_read(req);
		uv_close(req, ioreq_on_close);
	}
}

static void ioreq_killall(struct qr_task *task)
{
	for (size_t i = 0; i < task->pending_count; ++i) {
		ioreq_kill(task->pending[i]);
	}
	task->pending_count = 0;
}

/** @cond This memory layout is internal to mempool.c, use only for debugging. */
#if defined(__SANITIZE_ADDRESS__)
struct mempool_chunk {
  struct mempool_chunk *next;
  size_t size;
};
static void mp_poison(struct mempool *mp, bool poison)
{
	if (!poison) { /* @note mempool is part of the first chunk, unpoison it first */
		kr_asan_unpoison(mp, sizeof(*mp));
	}
	struct mempool_chunk *chunk = mp->state.last[0];
	void *chunk_off = (void *)chunk - chunk->size;
	if (poison) {
		kr_asan_poison(chunk_off, chunk->size);
	} else {
		kr_asan_unpoison(chunk_off, chunk->size);
	}
}
#else
#define mp_poison(mp, enable)
#endif
/** @endcond */

static inline struct mempool *pool_take(struct worker_ctx *worker)
{
	/* Recycle available mempool if possible */
	struct mempool *mp = NULL;
	if (worker->pools.len > 0) {
		mp = array_tail(worker->pools);
		array_pop(worker->pools);
	} else { /* No mempool on the freelist, create new one */
		mp = mp_new (4 * CPU_PAGE_SIZE);
	}
	mp_poison(mp, 0);
	return mp;
}

static inline void pool_release(struct worker_ctx *worker, struct mempool *mp)
{
	/* Return mempool to ring or free it if it's full */
	if (worker->pools.len < MP_FREELIST_SIZE) {
		mp_flush(mp);
		array_push(worker->pools, mp);
		mp_poison(mp, 1);
	} else {
		mp_delete(mp);
	}
}

static struct qr_task *qr_task_create(struct worker_ctx *worker, uv_handle_t *handle, knot_pkt_t *query, const struct sockaddr *addr)
{
	/* How much can client handle? */
	struct engine *engine = worker->engine;
	size_t answer_max = KNOT_WIRE_MIN_PKTSIZE;
	size_t pktbuf_max = KR_EDNS_PAYLOAD;
	if (engine->resolver.opt_rr) {
		pktbuf_max = MAX(knot_edns_get_payload(engine->resolver.opt_rr), pktbuf_max);
	}
	if (!addr && handle) { /* TCP */
		answer_max = KNOT_WIRE_MAX_PKTSIZE;
		pktbuf_max = KNOT_WIRE_MAX_PKTSIZE;
	} else if (knot_pkt_has_edns(query)) { /* EDNS */
		answer_max = MAX(knot_edns_get_payload(query->opt_rr), KNOT_WIRE_MIN_PKTSIZE);
	}

	/* Recycle available mempool if possible */
	knot_mm_t pool = {
		.ctx = pool_take(worker),
		.alloc = (knot_mm_alloc_t) mp_alloc
	};

	/* Create resolution task */
	struct qr_task *task = mm_alloc(&pool, sizeof(*task));
	if (!task) {
		mp_delete(pool.ctx);
		return NULL;
	}
	/* Create packet buffers for answer and subrequests */
	task->req.pool = pool;
	knot_pkt_t *pktbuf = knot_pkt_new(NULL, pktbuf_max, &task->req.pool);
	knot_pkt_t *answer = knot_pkt_new(NULL, answer_max, &task->req.pool);
	if (!pktbuf || !answer) {
		mp_delete(pool.ctx);
		return NULL;
	}
	task->req.answer = answer;
	task->pktbuf = pktbuf;
	array_init(task->waiting);
	task->addrlist = NULL;
	task->pending_count = 0;
	task->bytes_remaining = 0;
	task->iter_count = 0;
	task->refs = 1;
	task->finished = false;
	task->leading = false;
	task->worker = worker;
	task->source.handle = handle;
	uv_timer_init(worker->loop, &task->retry);
	uv_timer_init(worker->loop, &task->timeout);
	task->retry.data = task;
	task->timeout.data = task;
	task->on_complete = NULL;
	task->req.qsource.key = NULL;
	task->req.qsource.addr = NULL;
	/* Remember query source addr */
	if (addr) {
		size_t addr_len = sizeof(struct sockaddr_in);
		if (addr->sa_family == AF_INET6)
			addr_len = sizeof(struct sockaddr_in6);
		memcpy(&task->source.addr, addr, addr_len);
		task->req.qsource.addr = (const struct sockaddr *)&task->source.addr;
	} else {
		task->source.addr.ip4.sin_family = AF_UNSPEC;
	}
	/* Remember query source TSIG key */
	if (query->tsig_rr) {
		task->req.qsource.key = knot_rrset_copy(query->tsig_rr, &task->req.pool);
	}

	/* Start resolution */
	kr_resolve_begin(&task->req, &engine->resolver, answer);
	worker->stats.concurrent += 1;
	worker->stats.queries += 1;
	/* Throttle outbound queries only when high pressure */
	if (worker->stats.concurrent < QUERY_RATE_THRESHOLD) {
		task->req.options |= QUERY_NO_THROTTLE;
	}
	return task;
}

/* This is called when the task refcount is zero, free memory. */
static void qr_task_free(struct qr_task *task)
{
	/* Return mempool to ring or free it if it's full */
	struct worker_ctx *worker = task->worker;
	pool_release(worker, task->req.pool.ctx);
	/* @note The 'task' is invalidated from now on. */
	/* Decommit memory every once in a while */
	static int mp_delete_count = 0;
	if (++mp_delete_count == 100000) {
		lua_gc(worker->engine->L, LUA_GCCOLLECT, 0);
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
		malloc_trim(0);
#endif
		mp_delete_count = 0;
	}
}

/* This is called when retry timer closes */
static void retransmit_close(uv_handle_t *handle)
{
	struct qr_task *task = handle->data;
	qr_task_unref(task);
}

/* This is called when task completes and timeout timer is closed. */
static void qr_task_complete(uv_handle_t *handle)
{
	struct qr_task *task = handle->data;
	struct worker_ctx *worker = task->worker;
	/* Kill pending I/O requests */
	ioreq_killall(task);
	assert(task->waiting.len == 0);
	assert(task->leading == false);
	/* Run the completion callback. */
	if (task->on_complete) {
		task->on_complete(worker, &task->req, task->baton);
	}
	/* Return handle to the event loop in case
	 * it was exclusively taken by this task. */
	if (task->source.handle && !uv_has_ref(task->source.handle)) {
		uv_ref(task->source.handle);
		io_start_read(task->source.handle);
	}
	/* Release task */
	qr_task_unref(task);
	/* Update stats */
	worker->stats.concurrent -= 1;
}

/* This is called when I/O timeouts */
static void on_timeout(uv_timer_t *req)
{
	struct qr_task *task = req->data;
	uv_handle_t *handle = (uv_handle_t *)req;
#ifdef DEBUG
	char qname_str[KNOT_DNAME_MAXLEN] = {'\0'}, type_str[16] = {'\0'};
	knot_dname_to_str(qname_str, knot_pkt_qname(task->pktbuf), sizeof(qname_str));
	knot_rrtype_to_string(knot_pkt_qtype(task->pktbuf), type_str, sizeof(type_str));
	DEBUG_MSG("ioreq timeout %s %s %p\n", qname_str, type_str, req);
#endif
	/* Ignore if this timeout is being terminated. */
	if (uv_is_closing(handle)) {
		return;
	}
	/* Penalize all tried nameservers with a timeout. */
	struct worker_ctx *worker = task->worker;
	if (task->leading && task->pending_count > 0) {
		struct kr_query *qry = array_tail(task->req.rplan.pending);
		struct sockaddr_in6 *addrlist = (struct sockaddr_in6 *)task->addrlist;
		for (uint16_t i = 0; i < MIN(task->pending_count, task->addrlist_count); ++i) {
			struct sockaddr *choice = (struct sockaddr *)(&addrlist[i]);
			WITH_DEBUG {
				char addr_str[INET6_ADDRSTRLEN];
				inet_ntop(choice->sa_family, kr_inaddr(choice), addr_str, sizeof(addr_str));
				QRDEBUG(qry, "wrkr", "=> server: '%s' flagged as 'bad'\n", addr_str);
			}
			kr_nsrep_update_rtt(&qry->ns, choice, KR_NS_TIMEOUT, worker->engine->resolver.cache_rtt);
		}
	}
	/* Interrupt current pending request. */
	worker->stats.timeout += 1;
	qr_task_step(task, NULL, NULL);
}

/* This is called when we send subrequest / answer */
static int qr_task_on_send(struct qr_task *task, uv_handle_t *handle, int status)
{
	if (!task->finished) {
		if (status == 0 && handle) {
			io_start_read(handle); /* Start reading new query */
		} else {
			DEBUG_MSG("ioreq send_done %p => %d, %s\n", handle, status, uv_strerror(status));
		}
	} else {
		/* Close retry timer (borrows task) */
		qr_task_ref(task);
		uv_close((uv_handle_t *)&task->retry, retransmit_close);
		/* Close timeout timer (finishes task) */
		uv_close((uv_handle_t *)&task->timeout, qr_task_complete);
	}
	return status;
}

static void on_send(uv_udp_send_t *req, int status)
{
	struct worker_ctx *worker = get_worker();
	struct qr_task *task = req->data;
	if (qr_valid_handle(task, (uv_handle_t *)req->handle)) {
		qr_task_on_send(task, (uv_handle_t *)req->handle, status);
	}
	qr_task_unref(task);
	ioreq_release(worker, (struct ioreq *)req);
}

static void on_write(uv_write_t *req, int status)
{
	struct worker_ctx *worker = get_worker();
	struct qr_task *task = req->data;
	if (qr_valid_handle(task, (uv_handle_t *)req->handle)) {
		qr_task_on_send(task, (uv_handle_t *)req->handle, status);
	}
	qr_task_unref(task);
	ioreq_release(worker, (struct ioreq *)req);
}

static int qr_task_send(struct qr_task *task, uv_handle_t *handle, struct sockaddr *addr, knot_pkt_t *pkt)
{
	if (!handle) {
		return qr_task_on_send(task, handle, kr_error(EIO));
	}
	struct ioreq *send_req = ioreq_take(task->worker);
	if (!send_req) {
		return qr_task_on_send(task, handle, kr_error(ENOMEM));
	}

	/* Send using given protocol */
	int ret = 0;
	if (handle->type == UV_UDP) {
		uv_buf_t buf = { (char *)pkt->wire, pkt->size };
		send_req->as.send.data = task;
		ret = uv_udp_send(&send_req->as.send, (uv_udp_t *)handle, &buf, 1, addr, &on_send);
	} else {
		uint16_t pkt_size = htons(pkt->size);
		uv_buf_t buf[2] = {
			{ (char *)&pkt_size, sizeof(pkt_size) },
			{ (char *)pkt->wire, pkt->size }
		};
		send_req->as.write.data = task;
		ret = uv_write(&send_req->as.write, (uv_stream_t *)handle, buf, 2, &on_write);
	}
	if (ret == 0) {
		qr_task_ref(task); /* Pending ioreq on current task */
	} else {
		DEBUG_MSG("ioreq send_start %p => %d, %s\n", send_req, ret, uv_strerror(ret));
		ioreq_release(task->worker, send_req);
	}

	/* Update statistics */
	if (handle != task->source.handle && addr) {
		if (handle->type == UV_UDP)
			task->worker->stats.udp += 1;
		else
			task->worker->stats.tcp += 1;
		if (addr->sa_family == AF_INET6)
			task->worker->stats.ipv6 += 1;
		else
			task->worker->stats.ipv4 += 1;
	}
	return ret;
}

static void on_connect(uv_connect_t *req, int status)
{
	struct worker_ctx *worker = get_worker();
	struct qr_task *task = req->data;
	uv_stream_t *handle = req->handle;
	if (qr_valid_handle(task, (uv_handle_t *)req->handle)) {
		if (status == 0) {
			qr_task_send(task, (uv_handle_t *)handle, NULL, task->pktbuf);
		} else {
			DEBUG_MSG("ioreq conn_done %p => %d, %s\n", req, status, uv_strerror(status));
			qr_task_step(task, task->addrlist, NULL);
		}
	}
	qr_task_unref(task);
	ioreq_release(worker, (struct ioreq *)req);
}

static bool retransmit(struct qr_task *task)
{
	if (task && task->addrlist) {
		uv_handle_t *subreq = ioreq_spawn(task, SOCK_DGRAM);
		if (subreq) { /* Create connection for iterative query */
			struct sockaddr_in6 *choice = &((struct sockaddr_in6 *)task->addrlist)[task->addrlist_turn];
			if (qr_task_send(task, subreq, (struct sockaddr *)choice, task->pktbuf) == 0) {
				task->addrlist_turn = (task->addrlist_turn + 1) % task->addrlist_count; /* Round robin */
				return true;
			}
		}
	}
	return false;
}

static void on_retransmit(uv_timer_t *req)
{
	if (uv_is_closing((uv_handle_t *)req))
		return;
	if (!retransmit(req->data)) {
		uv_timer_stop(req); /* Not possible to spawn request, stop trying */
	}
}

/** @internal Get key from current outstanding subrequest. */
static int subreq_key(char *dst, struct qr_task *task)
{
	assert(task);
	knot_pkt_t *pkt = task->pktbuf;
	assert(knot_wire_get_qr(pkt->wire) == false);
	return kr_rrkey(dst, knot_pkt_qname(pkt), knot_pkt_qtype(pkt), knot_pkt_qclass(pkt));
}

static void subreq_finalize(struct qr_task *task, const struct sockaddr *packet_source, knot_pkt_t *pkt)
{
	/* Close pending I/O requests */
	if (uv_is_active((uv_handle_t *)&task->retry))
		uv_timer_stop(&task->retry);
	if (uv_is_active((uv_handle_t *)&task->timeout))
		uv_timer_stop(&task->timeout);
	ioreq_killall(task);
	/* Clear from outstanding table. */
	if (!task->leading)
		return;
	char key[KR_RRKEY_LEN];
	int ret = subreq_key(key, task);
	if (ret > 0) {
		assert(map_get(&task->worker->outstanding, key) == task);
		map_del(&task->worker->outstanding, key);
	}
	/* Notify waiting tasks. */
	struct kr_query *leader_qry = array_tail(task->req.rplan.pending);
	for (size_t i = task->waiting.len; i --> 0;) {
		struct qr_task *follower = task->waiting.at[i];
		struct kr_query *qry = array_tail(follower->req.rplan.pending);
		/* Reuse MSGID and 0x20 secret */
		if (qry) {
			qry->id = leader_qry->id;
			qry->secret = leader_qry->secret;
			leader_qry->secret = 0; /* Next will be already decoded */
		}
		qr_task_step(follower, packet_source, pkt);
		qr_task_unref(follower);
	}
	task->waiting.len = 0;
	task->leading = false;
}

static void subreq_lead(struct qr_task *task)
{
	assert(task);
	char key[KR_RRKEY_LEN];
	if (subreq_key(key, task) > 0) {
		assert(map_contains(&task->worker->outstanding, key) == false);
		map_set(&task->worker->outstanding, key, task);
		task->leading = true;
	}
}

static bool subreq_enqueue(struct qr_task *task)
{
	assert(task);
	char key[KR_RRKEY_LEN];
	if (subreq_key(key, task) > 0) {
		struct qr_task *leader = map_get(&task->worker->outstanding, key);
		if (leader) {
			/* Enqueue itself to leader for this subrequest. */
			int ret = array_reserve_mm(leader->waiting, leader->waiting.len + 1, kr_memreserve, &leader->req.pool);
			if (ret == 0) {
				array_push(leader->waiting, task);
				qr_task_ref(task);
				return true;
			}
		}
	}
	return false;
}

static int qr_task_finalize(struct qr_task *task, int state)
{
	assert(task && task->leading == false);
	kr_resolve_finish(&task->req, state);
	task->finished = true;
	/* Send back answer */
	(void) qr_task_send(task, task->source.handle, (struct sockaddr *)&task->source.addr, task->req.answer);
	return state == KNOT_STATE_DONE ? 0 : kr_error(EIO);
}

static int qr_task_step(struct qr_task *task, const struct sockaddr *packet_source, knot_pkt_t *packet)
{
	/* No more steps after we're finished. */
	if (task->finished) {
		return kr_error(ESTALE);
	}
	/* Close pending I/O requests */
	subreq_finalize(task, packet_source, packet);
	/* Consume input and produce next query */
	int sock_type = -1;
	task->addrlist = NULL;
	task->addrlist_count = 0;
	task->addrlist_turn = 0;
	int state = kr_resolve_consume(&task->req, packet_source, packet);
	while (state == KNOT_STATE_PRODUCE) {
		state = kr_resolve_produce(&task->req, &task->addrlist, &sock_type, task->pktbuf);
		if (unlikely(++task->iter_count > KR_ITER_LIMIT)) {
			DEBUG_MSG("task iter_limit %p\n", task);
			return qr_task_finalize(task, KNOT_STATE_FAIL);
		}
	}

	/* We're done, no more iterations needed */
	if (state & (KNOT_STATE_DONE|KNOT_STATE_FAIL)) {
		return qr_task_finalize(task, state);
	} else if (!task->addrlist || sock_type < 0) {
		return qr_task_step(task, NULL, NULL);
	}

	/* Count available address choices */
	struct sockaddr_in6 *choice = (struct sockaddr_in6 *)task->addrlist;
	for (size_t i = 0; i < KR_NSREP_MAXADDR && choice->sin6_family != AF_UNSPEC; ++i) {
		task->addrlist_count += 1;
		choice += 1;
	}

	/* Start fast retransmit with UDP, otherwise connect. */
	if (sock_type == SOCK_DGRAM) {
		/* If such subrequest is outstanding, enqueue to it. */
		if (subreq_enqueue(task)) {
			return kr_ok(); /* Will be notified when outstanding subrequest finishes. */
		}
		/* Start transmitting */
		if (retransmit(task)) {
			uv_timer_start(&task->retry, on_retransmit, KR_CONN_RETRY, KR_CONN_RETRY);
		} else {
			return qr_task_step(task, NULL, NULL);
		}
		/* Announce and start subrequest.
		 * @note Only UDP can lead I/O as it doesn't touch 'task->pktbuf' for reassembly.
		 */
		subreq_lead(task);
	} else {
		struct ioreq *conn = ioreq_take(task->worker);
		if (!conn) {
			return qr_task_step(task, NULL, NULL);
		}
		uv_handle_t *client = ioreq_spawn(task, sock_type);
		if (!client) {
			ioreq_release(task->worker, conn);
			return qr_task_step(task, NULL, NULL);
		}
		conn->as.connect.data = task;
		if (uv_tcp_connect(&conn->as.connect, (uv_tcp_t *)client, task->addrlist, on_connect) != 0) {
			ioreq_release(task->worker, conn);
			return qr_task_step(task, NULL, NULL);
		}
		/* Connect request borrows task */
		qr_task_ref(task);
	}

	/* Start next step with timeout, fatal if can't start a timer. */
	int ret = uv_timer_start(&task->timeout, on_timeout, KR_CONN_RTT_MAX, 0);
	if (ret != 0) {
		subreq_finalize(task, packet_source, packet);
		return qr_task_finalize(task, KNOT_STATE_FAIL);
	}

	return ret;
}

static int parse_packet(knot_pkt_t *query)
{
	if (!query)
		return kr_error(EINVAL);

	/* Parse query packet. */
	int ret = knot_pkt_parse(query, 0);
	if (ret != KNOT_EOK) {
		return kr_error(EPROTO); /* Ignore malformed query. */
	}

	/* Check if at least header is parsed. */
	if (query->parsed < query->size) {
		return kr_error(EMSGSIZE);
	}

	return kr_ok();
}

int worker_exec(struct worker_ctx *worker, uv_handle_t *handle, knot_pkt_t *query, const struct sockaddr* addr)
{
	if (!worker || !handle) {
		return kr_error(EINVAL);
	}

	/* Parse packet */
	int ret = parse_packet(query);

	/* Start new task on master sockets, or resume existing */
	struct qr_task *task = handle->data;
	bool is_master_socket = (!task);
	if (is_master_socket) {
		/* Ignore badly formed queries or responses. */
		if (!query || ret != 0 || knot_wire_get_qr(query->wire)) {
			DEBUG_MSG("task bad_query %p => %d, %s\n", task, ret, kr_strerror(ret));
			worker->stats.dropped += 1;
			return kr_error(EINVAL); /* Ignore. */
		}
		task = qr_task_create(worker, handle, query, addr);
		if (!task) {
			return kr_error(ENOMEM);
		}
	}

	/* Consume input and produce next query */
	return qr_task_step(task, addr, query);
}

/* Return DNS/TCP message size. */
static int msg_size(const uint8_t *msg, size_t len)
{
		if (len < 2) {
			return kr_error(EMSGSIZE);
		}
		return wire_read_u16(msg);
}

int worker_process_tcp(struct worker_ctx *worker, uv_handle_t *handle, const uint8_t *msg, size_t len)
{
	if (!worker || !handle || !msg) {
		return kr_error(EINVAL);
	}

	int nbytes = msg_size(msg, len);
	struct qr_task *task = handle->data;
	const bool start_assembly = (task && task->bytes_remaining == 0);

	/* Message is a query (we have no context to buffer it) or complete. */
	if (!task || (start_assembly && nbytes == len - 2)) {
		if (nbytes <= 0) {
			return worker_exec(worker, (uv_handle_t *)handle, NULL, NULL);	
		}
		knot_pkt_t *pkt_nocopy = knot_pkt_new((void *)(msg + 2), nbytes, &worker->pkt_pool);
		return worker_exec(worker, handle, pkt_nocopy, NULL);
	}
	/* Starting a new message assembly */
	knot_pkt_t *pkt_buf = task->pktbuf;
	if (start_assembly) {
		if (nbytes <= 0) {
			return worker_exec(worker, (uv_handle_t *)handle, NULL, NULL);	
		}	
		knot_pkt_clear(pkt_buf);
		pkt_buf->size = 0;
		/* Cut off message length */
		task->bytes_remaining = nbytes;
		len -= 2;
		msg += 2;
	}
	/* Message is too long, can't process it. */
	if (len > pkt_buf->max_size - pkt_buf->size) {
		task->bytes_remaining = 0;
		return worker_exec(worker, handle, NULL, NULL);
	}
	/* Buffer message and check if it's complete */
	memcpy(pkt_buf->wire + pkt_buf->size, msg, len);
	pkt_buf->size += len;
	if (len >= task->bytes_remaining) {
		task->bytes_remaining = 0;
		return worker_exec(worker, handle, pkt_buf, NULL);
	}
	/* Return number of bytes remaining to receive. */
	task->bytes_remaining -= len;
	return task->bytes_remaining;
}

int worker_resolve(struct worker_ctx *worker, knot_pkt_t *query, unsigned options, worker_cb_t on_complete, void *baton)
{
	if (!worker || !query) {
		return kr_error(EINVAL);
	}

	/* Create task */
	struct qr_task *task = qr_task_create(worker, NULL, query, NULL);
	if (!task) {
		return kr_error(ENOMEM);
	}
	task->baton = baton;
	task->on_complete = on_complete;
	task->req.options |= options;
	return qr_task_step(task, NULL, query);
}

int worker_reserve(struct worker_ctx *worker, size_t ring_maxlen)
{
	array_init(worker->pools);
	array_init(worker->ioreqs);
	if (array_reserve(worker->pools, ring_maxlen) || array_reserve(worker->ioreqs, ring_maxlen))
		return kr_error(ENOMEM);
	memset(&worker->pkt_pool, 0, sizeof(worker->pkt_pool));
	worker->pkt_pool.ctx = mp_new (4 * sizeof(knot_pkt_t));
	worker->pkt_pool.alloc = (knot_mm_alloc_t) mp_alloc;
	worker->outstanding = map_make();
	return kr_ok();
}

#define reclaim_freelist(list, type, cb) \
	for (unsigned i = 0; i < list.len; ++i) { \
		type *elm = list.at[i]; \
		kr_asan_unpoison(elm, sizeof(type)); \
		cb(elm); \
	} \
	array_clear(list)

void worker_reclaim(struct worker_ctx *worker)
{
	reclaim_freelist(worker->pools, struct mempool, mp_delete);
	reclaim_freelist(worker->ioreqs, struct ioreq, free);
	mp_delete(worker->pkt_pool.ctx);
	worker->pkt_pool.ctx = NULL;
	map_clear(&worker->outstanding);
}

#undef DEBUG_MSG
