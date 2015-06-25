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

#pragma once

#include <hiredis/hiredis.h>
#include "lib/generic/array.h"

/** Redis buffer size */
#define REDIS_BUFSIZE (512 * 1024)
#define REDIS_PORT 6379

typedef array_t(redisReply *) redis_freelist_t;

/** @internal Redis client */
struct redis_cli {
	redisContext *handle;
	redis_freelist_t freelist;
	char *addr;
	unsigned database;
	unsigned port;
};
