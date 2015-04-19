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

#include <stdio.h>

#include <libknot/internal/mempool.h>
#include <libknot/processing/requestor.h>
#include <libknot/rrtype/rdname.h>
#include <libknot/descriptor.h>
#include <dnssec/random.h>

#include "lib/rplan.h"
#include "lib/resolve.h"
#include "lib/layer/itercache.h"
#include "lib/layer/iterate.h"

#define DEBUG_MSG(fmt...) QRDEBUG(kr_rplan_current(param->rplan), "resl",  fmt)

/* Defines */
#define ITER_LIMIT 50

/** Invalidate current NS/addr pair. */
static int invalidate_ns(struct kr_rplan *rplan, struct kr_query *qry)
{
	uint8_t *addr = kr_nsrep_inaddr(qry->ns.addr);
	size_t addr_len = kr_nsrep_inaddr_len(qry->ns.addr);
	knot_rdata_t rdata[knot_rdata_array_size(addr_len)];
	knot_rdata_init(rdata, addr_len, addr, 0);
	return kr_zonecut_del(&qry->zone_cut, qry->ns.name, rdata);
}

static int ns_resolve_addr(struct kr_query *cur, struct kr_layer_param *param)
{
	if (kr_rplan_satisfies(cur, cur->ns.name, KNOT_CLASS_IN, KNOT_RRTYPE_A) ||
	    kr_rplan_satisfies(cur, cur->ns.name, KNOT_CLASS_IN, KNOT_RRTYPE_AAAA) ||
	    cur->flags & QUERY_AWAIT_ADDR) {
		DEBUG_MSG("=> dependency loop, bailing out\n");
		kr_rplan_pop(param->rplan, cur);
		return KNOT_EOK;
	}

	(void) kr_rplan_push(param->rplan, cur, cur->ns.name, KNOT_CLASS_IN, KNOT_RRTYPE_AAAA);
	(void) kr_rplan_push(param->rplan, cur, cur->ns.name, KNOT_CLASS_IN, KNOT_RRTYPE_A);
	cur->flags |= QUERY_AWAIT_ADDR;
	return KNOT_EOK;
}

static int iterate(struct knot_requestor *requestor, struct kr_layer_param *param)
{
	int ret = KNOT_EOK;
	struct timeval timeout = { KR_CONN_RTT_MAX / 1000, 0 };
	struct kr_rplan *rplan = param->rplan;
	struct kr_query *cur = kr_rplan_current(rplan);

#ifndef NDEBUG
	char name_str[KNOT_DNAME_MAXLEN], type_str[16];
	knot_dname_to_str(name_str, cur->sname, sizeof(name_str));
	knot_rrtype_to_string(cur->stype, type_str, sizeof(type_str));
	DEBUG_MSG("query '%s %s'\n", name_str, type_str);
#endif

	/* Elect best nameserver candidate. */
	kr_nsrep_elect(&cur->ns, &cur->zone_cut.nsset);
	if (cur->ns.score < KR_NS_VALID) {
		DEBUG_MSG("=> no valid NS left\n");
		kr_rplan_pop(param->rplan, cur);
		return KNOT_EOK;
	} else {
		if (cur->ns.addr.ip.sa_family == AF_UNSPEC) {
			DEBUG_MSG("=> ns missing A/AAAA, fetching\n");
			return ns_resolve_addr(cur, param);
		}
	}

	/* Prepare query resolution. */
	int mode = (cur->flags & QUERY_TCP) ? 0 : KNOT_RQ_UDP;
	knot_pkt_t *query = knot_pkt_new(NULL, KNOT_WIRE_MIN_PKTSIZE, requestor->mm);
	struct knot_request *tx = knot_request_make(requestor->mm, &cur->ns.addr.ip, NULL, query, mode);
	knot_requestor_enqueue(requestor, tx);

	/* Resolve and check status. */
	ret = knot_requestor_exec(requestor, &timeout);
	if (ret != KNOT_EOK) {
		/* Network error, retry over TCP. */
		if (ret != KNOT_LAYER_ERROR && !(cur->flags & QUERY_TCP)) {
			DEBUG_MSG("=> ns unreachable, retrying over TCP\n");
			cur->flags |= QUERY_TCP;
			return iterate(requestor, param);
		}
		/* Resolution failed, invalidate current NS and reset to UDP. */
		DEBUG_MSG("=> resolution failed: '%s', invalidating\n", knot_strerror(ret));
		if (invalidate_ns(rplan, cur) == 0) {
			cur->flags &= ~QUERY_TCP;
		}
		return KNOT_EOK;
	}

	/* Pop query if resolved. */
	if (cur->flags & QUERY_RESOLVED) {
		kr_rplan_pop(rplan, cur);
	}

	return ret;
}

static void prepare_layers(struct knot_requestor *req, struct kr_layer_param *param)
{
	struct kr_context *ctx = param->ctx;
	for (size_t i = 0; i < ctx->modules->len; ++i) {
		struct kr_module *mod = &ctx->modules->at[i];
		if (mod->layer) {
			knot_requestor_overlay(req, mod->layer(), param);
		}
	}
}

static int resolve_iterative(struct kr_layer_param *param, mm_ctx_t *pool)
{
/* Initialize requestor. */
	struct knot_requestor requestor;
	knot_requestor_init(&requestor, pool);
	prepare_layers(&requestor, param);

	/* Iteratively solve the query. */
	int ret = KNOT_EOK;
	unsigned iter_count = 0;
	while((ret == KNOT_EOK) && !kr_rplan_empty(param->rplan)) {
		ret = iterate(&requestor, param);
		if (++iter_count > ITER_LIMIT) {
			DEBUG_MSG("iteration limit %d reached\n", ITER_LIMIT);
			ret = KNOT_ELIMIT;
		}
	}

	/* Set RCODE on internal failure. */
	if (ret != KNOT_EOK) {
		if (knot_wire_get_rcode(param->answer->wire) == KNOT_RCODE_NOERROR) {
			knot_wire_set_rcode(param->answer->wire, KNOT_RCODE_SERVFAIL);
		}
	}

	DEBUG_MSG("finished: %s, mempool: %llu B\n", knot_strerror(ret), mp_total_size(pool->ctx));
	knot_requestor_clear(&requestor);
	return ret;
}

int kr_resolve(struct kr_context* ctx, knot_pkt_t *answer,
               const knot_dname_t *qname, uint16_t qclass, uint16_t qtype)
{
	if (ctx == NULL || answer == NULL || qname == NULL) {
		return KNOT_EINVAL;
	}

	/* Initialize context. */
	int ret = KNOT_EOK;
	mm_ctx_t rplan_pool;
	mm_ctx_mempool(&rplan_pool, MM_DEFAULT_BLKSIZE);
	struct kr_rplan rplan;
	kr_rplan_init(&rplan, ctx, &rplan_pool);
	struct kr_layer_param param;
	param.ctx = ctx;
	param.rplan = &rplan;
	param.answer = answer;

	/* Push query to resolution plan. */
	struct kr_query *qry = kr_rplan_push(&rplan, NULL, qname, qclass, qtype);
	if (qry != NULL) {
		ret = resolve_iterative(&param, &rplan_pool);
	} else {
		ret = KNOT_ENOMEM;
	}

	/* Check flags. */
	knot_wire_set_qr(answer->wire);
	knot_wire_clear_aa(answer->wire);
	knot_wire_set_ra(answer->wire);

	/* Resolution success, commit cache transaction. */
	if (ret == KNOT_EOK) {
		kr_rplan_txn_commit(&rplan);
	}

	/* Clean up. */
	kr_rplan_deinit(&rplan);
	mp_delete(rplan_pool.ctx);

	return ret;
}
