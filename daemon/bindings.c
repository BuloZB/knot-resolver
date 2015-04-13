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

#include <uv.h>

#include "lib/cache.h"
#include "daemon/bindings.h"

/** @internal Compatibility wrapper for Lua 5.0 - 5.2 */
#if LUA_VERSION_NUM >= 502
#define register_lib(L, name, lib) \
	luaL_newlib((L), (lib))
#else
#define lua_rawlen(L, obj) \
	lua_objlen((L), (obj))
#define register_lib(L, name, lib) \
	luaL_openlib((L), (name), (lib), 0)
#endif

/** List loaded modules */
static int mod_list(lua_State *L)
{
	struct engine *engine = engine_luaget(L);
	lua_newtable(L);
	for (unsigned i = 0; i < engine->modules.len; ++i) {
		struct kr_module *module = &engine->modules.at[i];
		lua_pushstring(L, module->name);
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

/** Load module. */
static int mod_load(lua_State *L)
{
	/* Check parameters */
	int n = lua_gettop(L);
	if (n != 1 || !lua_isstring(L, 1)) {
		lua_pushstring(L, "expected module name");
		lua_error(L);
	}
	/* Load engine module */
	struct engine *engine = engine_luaget(L);
	int ret = engine_register(engine, lua_tostring(L, 1));
	if (ret != 0) {
		lua_pushstring(L, kr_strerror(ret));
		lua_error(L);
	}

	lua_pushboolean(L, 1);
	return 1;
}

/** Unload module. */
static int mod_unload(lua_State *L)
{
	/* Check parameters */
	int n = lua_gettop(L);
	if (n != 1 || !lua_isstring(L, 1)) {
		lua_pushstring(L, "expected module name");
		lua_error(L);
	}
	/* Unload engine module */
	struct engine *engine = engine_luaget(L);
	int ret = engine_unregister(engine, lua_tostring(L, 1));
	if (ret != 0) {
		lua_pushstring(L, kr_strerror(ret));
		lua_error(L);
	}

	lua_pushboolean(L, 1);
	return 1;
}

int lib_modules(lua_State *L)
{
	static const luaL_Reg lib[] = {
		{ "list",   mod_list },
		{ "load",   mod_load },
		{ "unload", mod_unload },
		{ NULL, NULL }
	};

	register_lib(L, "modules", lib);
	return 1;
}

/** Append 'addr = {port = int, udp = bool, tcp = bool}' */
static int net_list_add(const char *key, void *val, void *ext)
{
	lua_State *L = (lua_State *)ext;
	endpoint_array_t *ep_array = val;
	lua_newtable(L);
	for (size_t i = ep_array->len; i--;) {
		struct endpoint *ep = ep_array->at[i];
		lua_pushinteger(L, ep->port);
		lua_setfield(L, -2, "port");
		lua_pushboolean(L, ep->flags & NET_UDP);
		lua_setfield(L, -2, "udp");
		lua_pushboolean(L, ep->flags & NET_TCP);
		lua_setfield(L, -2, "tcp");
	}
	lua_setfield(L, -2, key);
	return kr_ok();
}

/** List active endpoints. */
static int net_list(lua_State *L)
{
	struct engine *engine = engine_luaget(L);
	lua_newtable(L);
	map_walk(&engine->net.endpoints, net_list_add, L);
	return 1;
}

/** Listen on interface address list. */
static int net_listen_iface(lua_State *L, int port)
{
	/* Expand 'addr' key if exists */
	lua_getfield(L, 1, "addr");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_pushvalue(L, 1);
	}

	/* Bind to address list */
	struct engine *engine = engine_luaget(L);
	size_t count = lua_rawlen(L, -1);
	for (size_t i = 0; i < count; ++i) {
		lua_rawgeti(L, -1, i + 1);
		int ret = network_listen(&engine->net, lua_tostring(L, -1),
		                         port, NET_TCP|NET_UDP);
		if (ret != 0) {
			lua_pushstring(L, kr_strerror(ret));
			lua_error(L);
		}
		lua_pop(L, 1);
	}

	lua_pushboolean(L, true);
	return 1;
}

/** Listen on endpoint. */
static int net_listen(lua_State *L)
{
	/* Check parameters */
	int n = lua_gettop(L);
	int port = KR_DNS_PORT;
	if (n > 1 && lua_isnumber(L, 2)) {
		port = lua_tointeger(L, 2);
	}

	/* Process interface or (address, port) pair. */
	if (lua_istable(L, 1)) {
		return net_listen_iface(L, port);
	} else if (n < 1 || !lua_isstring(L, 1)) {
		lua_pushstring(L, "expected (string addr, int port = 53)");
		lua_error(L);
	}

	/* Open resolution context cache */
	struct engine *engine = engine_luaget(L);
	int ret = network_listen(&engine->net, lua_tostring(L, 1), port, NET_TCP|NET_UDP);
	if (ret != 0) {
		lua_pushstring(L, kr_strerror(ret));
		lua_error(L);
	}

	lua_pushboolean(L, true);
	return 1;
}

/** Close endpoint. */
static int net_close(lua_State *L)
{
	/* Check parameters */
	int n = lua_gettop(L);
	if (n < 2) {
		lua_pushstring(L, "expected (string addr, int port)");
		lua_error(L);
	}

	/* Open resolution context cache */
	struct engine *engine = engine_luaget(L);
	int ret = network_close(&engine->net, lua_tostring(L, 1), lua_tointeger(L, 2));
	lua_pushboolean(L, ret == 0);
	return 1;
}

/** List available interfaces. */
static int net_interfaces(lua_State *L)
{
	/* Retrieve interface list */
	int count = 0;
	char buf[INET6_ADDRSTRLEN]; /* http://tools.ietf.org/html/rfc4291 */
	uv_interface_address_t *info = NULL;
	uv_interface_addresses(&info, &count);
	lua_newtable(L);
	for (int i = 0; i < count; ++i) {
		uv_interface_address_t iface = info[i];
		lua_getfield(L, -1, iface.name);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			lua_newtable(L);
		}

		/* Address */
		lua_getfield(L, -1, "addr");
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			lua_newtable(L);
		}
		if (iface.address.address4.sin_family == AF_INET) {
			uv_ip4_name(&iface.address.address4, buf, sizeof(buf));
		} else if (iface.address.address4.sin_family == AF_INET6) {
			uv_ip6_name(&iface.address.address6, buf, sizeof(buf));
		} else {
			buf[0] = '\0';
		}
		lua_pushstring(L, buf);
		lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
		lua_setfield(L, -2, "addr");

		/* Hardware address. */
		char *p = buf;
		memset(buf, 0, sizeof(buf));
		for (unsigned k = 0; k < sizeof(iface.phys_addr); ++k) {
			sprintf(p, "%.2x:", iface.phys_addr[k] & 0xff);
			p += 3;
		}
		*(p - 1) = '\0';
		lua_pushstring(L, buf);
		lua_setfield(L, -2, "mac");

		/* Push table */
		lua_setfield(L, -2, iface.name);
	}
	uv_free_interface_addresses(info, count);

	return 1;
}

int lib_net(lua_State *L)
{
	static const luaL_Reg lib[] = {
		{ "list",       net_list },
		{ "listen",     net_listen },
		{ "close",      net_close },
		{ "interfaces", net_interfaces },
		{ NULL, NULL }
	};
	register_lib(L, "net", lib);
	return 1;
}

/** Return number of cached records. */
static int cache_count(lua_State *L)
{
	struct engine *engine = engine_luaget(L);
	const namedb_api_t *storage = kr_cache_storage();

	/* Fetch item count */
	namedb_txn_t txn;
	int ret = kr_cache_txn_begin(engine->resolver.cache, &txn, NAMEDB_RDONLY);
	if (ret != 0) {
		lua_pushstring(L, kr_strerror(ret));
		lua_error(L);
	}

	lua_pushinteger(L, storage->count(&txn));
	kr_cache_txn_abort(&txn);
	return 1;
}

/** Open cache */
static int cache_open(lua_State *L)
{
	/* Check parameters */
	int n = lua_gettop(L);
	if (n < 1) {
		lua_pushstring(L, "expected (number max_size)");
		lua_error(L);
	}

	/* Close if already open */
	struct engine *engine = engine_luaget(L);
	if (engine->resolver.cache != NULL) {
		kr_cache_close(engine->resolver.cache);
	}

	/* Open resolution context cache */
	engine->resolver.cache = kr_cache_open(".", engine->pool, lua_tointeger(L, 1));
	if (engine->resolver.cache == NULL) {
		lua_pushstring(L, "can't open cache in rundir");
		lua_error(L);
	}

	lua_pushboolean(L, 1);
	return 1;
}

static int cache_close(lua_State *L)
{
	struct engine *engine = engine_luaget(L);
	if (engine->resolver.cache != NULL) {
		struct kr_cache *cache = engine->resolver.cache;
		engine->resolver.cache = NULL;
		kr_cache_close(cache);
	}

	lua_pushboolean(L, 1);
	return 1;
}

int lib_cache(lua_State *L)
{
	static const luaL_Reg lib[] = {
		{ "count",  cache_count },
		{ "open",   cache_open },
		{ "close",  cache_close },
		{ NULL, NULL }
	};

	register_lib(L, "cache", lib);
	return 1;
}
