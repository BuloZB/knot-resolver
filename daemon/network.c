/*  Copyright (C) 2015 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#include "daemon/network.h"
#include "daemon/worker.h"
#include "daemon/io.h"

void network_init(struct network *net, uv_loop_t *loop)
{
	if (net != NULL) {
		/* No multiplexing now, I/O in single thread. */
		net->loop = loop;
		net->endpoints = map_make();
	}
}

/** Close endpoint protocols. */
static int close_endpoint(struct endpoint *ep)
{
	if (ep->flags & NET_UDP) {
		udp_unbind(ep);
	}
	if (ep->flags & NET_TCP) {
		tcp_unbind(ep);
	}

	free(ep);
	return kr_ok();
}

/** Endpoint visitor (see @file map.h) */
static int visit_key(const char *key, void *val, void *ext)
{
	int (*callback)(struct endpoint *) = ext;
	endpoint_array_t *ep_array = val;
	for (size_t i = ep_array->len; i--;) {
		callback(ep_array->at[i]);
	}
	return 0;
}

static int free_key(const char *key, void *val, void *ext)
{
	endpoint_array_t *ep_array = val;
	array_clear(*ep_array);
	free(ep_array);
	return kr_ok();
}

void network_deinit(struct network *net)
{
	if (net != NULL) {
		map_walk(&net->endpoints, visit_key, close_endpoint);
		map_walk(&net->endpoints, free_key, 0);
		map_clear(&net->endpoints);
	}
}

/** Fetch or create endpoint array and insert endpoint. */
static int insert_endpoint(struct network *net, const char *addr, struct endpoint *ep)
{
	/* Fetch or insert address into map */
	endpoint_array_t *ep_array = map_get(&net->endpoints, addr);
	if (ep_array == NULL) {
		ep_array = malloc(sizeof(*ep_array));
		if (ep_array == NULL) {
			return kr_error(ENOMEM);
		}
		if (map_set(&net->endpoints, addr, ep_array) != 0) {
			free(ep_array);
			return kr_error(ENOMEM);
		}
		array_init(*ep_array);
	}

	return array_push(*ep_array, ep);
}

/** Open endpoint protocols. */
static int open_endpoint(struct network *net, struct endpoint *ep, struct sockaddr *sa, uint32_t flags)
{
	if (flags & NET_UDP) {
		uv_udp_init(net->loop, &ep->udp);
		int ret = udp_bind(ep, sa);
		if (ret != 0) {
			return ret;
		}
		ep->flags |= NET_UDP;
	}
	if (flags & NET_TCP) {
		uv_tcp_init(net->loop, &ep->tcp);
		int ret = tcp_bind(ep, sa);
		if (ret != 0) {
			return ret;
		}
		ep->flags |= NET_TCP;
	}
	return kr_ok();
}

int network_listen(struct network *net, const char *addr, uint16_t port, uint32_t flags)
{
	if (net == NULL || addr == 0 || port == 0) {
		return kr_error(EINVAL);
	}

	/* Parse address. */
	int ret = 0;
	struct sockaddr_storage sa;
	if (strchr(addr, ':') != NULL) {
		ret = uv_ip6_addr(addr, port, (struct sockaddr_in6 *)&sa);
	} else {
		ret = uv_ip4_addr(addr, port, (struct sockaddr_in *)&sa);
	}
	if (ret != 0) {
		return ret;
	}

	/* Bind interfaces */
	struct endpoint *ep = malloc(sizeof(*ep));
	memset(ep, 0, sizeof(*ep));
	ep->flags = NET_DOWN;
	ep->port = port;
	ret = open_endpoint(net, ep, (struct sockaddr *)&sa, flags);
	if (ret == 0) {
		ret = insert_endpoint(net, addr, ep);
	}
	if (ret != 0) {
		close_endpoint(ep);
	}

	return ret;
}

int network_close(struct network *net, const char *addr, uint16_t port)
{
	endpoint_array_t *ep_array = map_get(&net->endpoints, addr);
	if (ep_array == NULL) {
		return kr_error(ENOENT);
	}

	/* Close endpoint in array. */
	for (size_t i = ep_array->len; i--;) {
		struct endpoint *ep = ep_array->at[i];
		if (ep->port == port) {
			close_endpoint(ep);
			array_del(*ep_array, i);
			break;
		}
	}

	/* Collapse key if it has no endpoint. */
	if (ep_array->len == 0) {
		free(ep_array);
		map_del(&net->endpoints, addr);
	}

	return kr_ok();
}
