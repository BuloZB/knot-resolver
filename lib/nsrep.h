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

/** \addtogroup nameservers
 * @{
 */

#pragma once

#include "lib/layer.h"

enum kr_ns_score {
    KR_NS_INVALID = -1,
    KR_NS_VALID   = 0
};

/** Return name server score (KR_NS_VALID is baseline, the higher the better).
 * @param ns evaluated NS name
 * @param param layer parameters
 * @return enum kr_ns_score or higher positive value
 */
int kr_nsrep_score(const knot_dname_t *ns, struct kr_layer_param *param);

/** @} */
