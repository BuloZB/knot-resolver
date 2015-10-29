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

#include "lib/generic/map.h"
#include <libknot/rrset.h>

/**
 * Find TA RRSet by name.
 * @param  trust_anchors trust store
 * @param  name          name of the TA
 * @return non-empty RRSet or NULL
 */
knot_rrset_t *kr_ta_get(map_t *trust_anchors, const knot_dname_t *name);

/**
 * Add TA to trust store. DS or DNSKEY types are supported.
 * @param  trust_anchors trust store
 * @param  name          name of the TA
 * @param  type          RR type of the TA (DS or DNSKEY)
 * @param  ttl           
 * @param  rdata         
 * @param  rdlen         
 * @return 0 or an error
 */
int kr_ta_add(map_t *trust_anchors, const knot_dname_t *name, uint16_t type,
               uint32_t ttl, const uint8_t *rdata, uint16_t rdlen);

/**
 * Return true if the name is below/at any TA in the store.
 * This can be useful to check if it's possible to validate a name beforehand.
 * @param  trust_anchors trust store
 * @param  name          name of the TA
 * @return boolean
 */
int kr_ta_covers(map_t *trust_anchors, const knot_dname_t *name);

/**
 * Remove TA from trust store.
 * @param  trust_anchors trust store
 * @param  name          name of the TA
 * @return 0 or an error
 */
int kr_ta_del(map_t *trust_anchors, const knot_dname_t *name);

/**
 * Clear trust store.
 * @param trust_anchors trust store
 */
void kr_ta_clear(map_t *trust_anchors);
