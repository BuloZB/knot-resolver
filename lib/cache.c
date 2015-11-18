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

#include <assert.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include <libknot/internal/mempattern.h>
#include <libknot/internal/namedb/namedb_lmdb.h>
#include <libknot/errcode.h>
#include <libknot/descriptor.h>
#include <libknot/dname.h>
#include <libknot/rrtype/rrsig.h>

#include "lib/cache.h"
#include "lib/defines.h"
#include "lib/utils.h"

/* Cache version */
#define KEY_VERSION "V\x02"
/* Key size */
#define KEY_HSIZE (sizeof(uint8_t) + sizeof(uint16_t))
#define KEY_SIZE (KEY_HSIZE + KNOT_DNAME_MAXLEN)
#define txn_api(txn) ((txn)->owner->api)
#define txn_is_valid(txn) ((txn) && (txn)->owner && txn_api(txn))

/** @internal Check cache internal data version. Clear if it doesn't match. */
static void assert_right_version(struct kr_cache *cache)
{
	/* Check cache ABI version */
	struct kr_cache_txn txn;
	int ret = kr_cache_txn_begin(cache, &txn, 0);
	if (ret != 0) {
		return; /* N/A, doesn't work. */
	}
	namedb_val_t key = { KEY_VERSION, 2 };
	namedb_val_t val = { NULL, 0 };
	ret = txn_api(&txn)->find(&txn.t, &key, &val, 0);
	if (ret == 0) { /* Version is OK */
		kr_cache_txn_abort(&txn);
		return;
	}
	/* Recreate cache and write version key */
	ret = txn_api(&txn)->count(&txn.t);
	if (ret > 0) { /* Non-empty cache, purge it. */
		log_info("[cache] version mismatch, clearing\n");
		kr_cache_clear(&txn);
		kr_cache_txn_commit(&txn);
		ret = kr_cache_txn_begin(cache, &txn, 0);
	}
	/* Either purged or empty. */
	if (ret == 0) {
		txn_api(&txn)->insert(&txn.t, &key, &val, 0);
		kr_cache_txn_commit(&txn);
	}
}

int kr_cache_open(struct kr_cache *cache, const namedb_api_t *api, void *opts, mm_ctx_t *mm)
{
	if (!cache) {
		return kr_error(EINVAL);
	}
	/* Open cache */
	cache->api = (api == NULL) ? namedb_lmdb_api() : api;
	int ret = cache->api->init(&cache->db, mm, opts);
	if (ret != 0) {
		return ret;
	}
	memset(&cache->stats, 0, sizeof(cache->stats));
	/* Check cache ABI version */
	assert_right_version(cache);
	return kr_ok();
}

void kr_cache_close(struct kr_cache *cache)
{
	if (cache && cache->db) {
		if (cache->api) {
			cache->api->deinit(cache->db);
		}
		cache->db = NULL;
	}
}

int kr_cache_txn_begin(struct kr_cache *cache, struct kr_cache_txn *txn, unsigned flags)
{
	if (!cache || !cache->db || !cache->api || !txn ) {
		return kr_error(EINVAL);
	}
	/* Open new transaction */
	int ret = cache->api->txn_begin(cache->db, &txn->t, flags);
	if (ret != 0) {
		memset(txn, 0, sizeof(*txn));
	} else {
		/* Count statistics */
		txn->owner = cache;
		if (flags & NAMEDB_RDONLY) {
			cache->stats.txn_read += 1;
		} else {
			cache->stats.txn_write += 1;
		}
	}
	return ret;
}

int kr_cache_txn_commit(struct kr_cache_txn *txn)
{
	if (!txn_is_valid(txn)) {
		return kr_error(EINVAL);
	}

	int ret = txn_api(txn)->txn_commit(&txn->t);
	if (ret != 0) {
		kr_cache_txn_abort(txn);
	}
	return ret;
}

void kr_cache_txn_abort(struct kr_cache_txn *txn)
{
	if (txn_is_valid(txn)) {
		txn_api(txn)->txn_abort(&txn->t);
	}
}

/**
 * @internal Composed key as { u8 tag, u8[1-255] name, u16 type }
 * The name is lowercased and label order is reverted for easy prefix search.
 * e.g. '\x03nic\x02cz\x00' is saved as '\0x00cz\x00nic\x00'
 */
static size_t cache_key(uint8_t *buf, uint8_t tag, const knot_dname_t *name, uint16_t rrtype)
{
	/* Convert to lookup format */
	int ret = knot_dname_lf(buf, name, NULL);
	if (ret != 0) {
		return 0;
	}
	/* Write tag + type */
	uint8_t name_len = buf[0];
	buf[0] = tag;
	memcpy(buf + sizeof(uint8_t) + name_len, &rrtype, sizeof(uint16_t));
	return name_len + KEY_HSIZE;
}

static struct kr_cache_entry *lookup(struct kr_cache_txn *txn, uint8_t tag, const knot_dname_t *name, uint16_t type)
{
	uint8_t keybuf[KEY_SIZE];
	size_t key_len = cache_key(keybuf, tag, name, type);
	if (!txn_is_valid(txn) || !name) {
		return NULL;
	}

	/* Look up and return value */
	namedb_val_t key = { keybuf, key_len };
	namedb_val_t val = { NULL, 0 };
	int ret = txn_api(txn)->find(&txn->t, &key, &val, 0);
	if (ret != KNOT_EOK) {
		return NULL;
	}

	return (struct kr_cache_entry *)val.data;
}

static int check_lifetime(struct kr_cache_entry *found, uint32_t *timestamp)
{
	/* No time constraint */
	if (!timestamp) {
		return kr_ok();
	} else if (*timestamp <= found->timestamp) {
		/* John Connor record cached in the future. */
		*timestamp = 0;
		return kr_ok();
	} else {
		/* Check if the record is still valid. */
		uint32_t drift = *timestamp - found->timestamp;
		if (drift <= found->ttl) {
			*timestamp = drift;
			return kr_ok();
		}
	}
	return kr_error(ESTALE);
}

int kr_cache_peek(struct kr_cache_txn *txn, uint8_t tag, const knot_dname_t *name, uint16_t type,
                  struct kr_cache_entry **entry, uint32_t *timestamp)
{
	if (!txn_is_valid(txn) || !name || !entry) {
		return kr_error(EINVAL);
	}

	struct kr_cache_entry *found = lookup(txn, tag, name, type);
	if (!found) {
		txn->owner->stats.miss += 1;
		return kr_error(ENOENT);
	}

	/* Check entry lifetime */
	*entry = found;
	int ret = check_lifetime(found, timestamp);
	if (ret == 0) {
		txn->owner->stats.hit += 1;
	} else {
		txn->owner->stats.miss += 1;
	}
	return ret;
}

static void entry_write(struct kr_cache_entry *dst, struct kr_cache_entry *header, namedb_val_t data)
{
	assert(dst);
	memcpy(dst, header, sizeof(*header));
	memcpy(dst->data, data.data, data.len);
}

int kr_cache_insert(struct kr_cache_txn *txn, uint8_t tag, const knot_dname_t *name, uint16_t type,
                    struct kr_cache_entry *header, namedb_val_t data)
{
	if (!txn_is_valid(txn) || !name || !header) {
		return kr_error(EINVAL);
	}

	/* Insert key */
	uint8_t keybuf[KEY_SIZE];
	size_t key_len = cache_key(keybuf, tag, name, type);
	if (key_len == 0) {
		return kr_error(EILSEQ);
	}
	namedb_val_t key = { keybuf, key_len };
	namedb_val_t entry = { NULL, sizeof(*header) + data.len };
	const namedb_api_t *db_api = txn_api(txn);

	/* LMDB can do late write and avoid copy */
	txn->owner->stats.insert += 1;
	if (db_api == namedb_lmdb_api()) {
		int ret = db_api->insert(&txn->t, &key, &entry, 0);
		if (ret != 0) {
			return ret;
		}
		entry_write(entry.data, header, data);
	} else {
		/* Other backends must prepare contiguous data first */
		entry.data = malloc(entry.len);
		if (!entry.data) {
			return kr_error(ENOMEM);
		}
		entry_write(entry.data, header, data);
		int ret = db_api->insert(&txn->t, &key, &entry, 0);
		free(entry.data);
		if (ret != 0) {
			return ret;
		}
	}

	return kr_ok();
}

int kr_cache_remove(struct kr_cache_txn *txn, uint8_t tag, const knot_dname_t *name, uint16_t type)
{
	if (!txn_is_valid(txn) || !name ) {
		return kr_error(EINVAL);
	}

	uint8_t keybuf[KEY_SIZE];
	size_t key_len = cache_key(keybuf, tag, name, type);
	if (key_len == 0) {
		return kr_error(EILSEQ);
	}
	namedb_val_t key = { keybuf, key_len };
	txn->owner->stats.delete += 1;
	return txn_api(txn)->del(&txn->t, &key);
}

int kr_cache_clear(struct kr_cache_txn *txn)
{
	if (!txn_is_valid(txn)) {
		return kr_error(EINVAL);
	}

	return txn_api(txn)->clear(&txn->t);
}

int kr_cache_peek_rr(struct kr_cache_txn *txn, knot_rrset_t *rr, uint16_t *rank, uint32_t *timestamp)
{
	if (!txn_is_valid(txn) || !rr || !timestamp) {
		return kr_error(EINVAL);
	}

	/* Check if the RRSet is in the cache. */
	struct kr_cache_entry *entry = NULL;
	int ret = kr_cache_peek(txn, KR_CACHE_RR, rr->owner, rr->type, &entry, timestamp);
	if (ret != 0) {
		return ret;
	}
	if (rank) {
		*rank = entry->rank;
	}
	rr->rrs.rr_count = entry->count;
	rr->rrs.data = entry->data;
	return kr_ok();
}

int kr_cache_peek_rank(struct kr_cache_txn *txn, uint8_t tag, const knot_dname_t *name, uint16_t type, uint32_t timestamp)
{
	if (!txn_is_valid(txn) || !name) {
		return kr_error(EINVAL);
	}
	struct kr_cache_entry *found = lookup(txn, tag, name, type);
	if (!found) {
		return kr_error(ENOENT);
	}
	if (check_lifetime(found, &timestamp) != 0) {
		return kr_error(ESTALE);
	}
	return found->rank;
}

int kr_cache_materialize(knot_rrset_t *dst, const knot_rrset_t *src, uint32_t drift, mm_ctx_t *mm)
{
	if (!dst || !src) {
		return kr_error(EINVAL);
	}

	/* Make RRSet copy */
	knot_rrset_init(dst, NULL, src->type, src->rclass);
	dst->owner = knot_dname_copy(src->owner, mm);
	if (!dst->owner) {
		return kr_error(ENOMEM);
	}

	/* Copy valid records */
	knot_rdata_t *rd = src->rrs.data;
	for (uint16_t i = 0; i < src->rrs.rr_count; ++i) {
		if (knot_rdata_ttl(rd) >= drift) {
			if (knot_rdataset_add(&dst->rrs, rd, mm) != 0) {
				knot_rrset_clear(dst, mm);
				return kr_error(ENOMEM);
			}
		}
		rd = kr_rdataset_next(rd);
	}
	/* Fixup TTL by time passed */
	rd = dst->rrs.data;
	for (uint16_t i = 0; i < dst->rrs.rr_count; ++i) {
		knot_rdata_set_ttl(rd, knot_rdata_ttl(rd) - drift);
		rd = kr_rdataset_next(rd);
	}

	return kr_ok();
}

int kr_cache_insert_rr(struct kr_cache_txn *txn, const knot_rrset_t *rr, uint16_t rank, uint32_t timestamp)
{
	if (!txn_is_valid(txn) || !rr) {
		return kr_error(EINVAL);
	}

	/* Ignore empty records */
	if (knot_rrset_empty(rr)) {
		return kr_ok();
	}

	/* Prepare header to write */
	struct kr_cache_entry header = {
		.timestamp = timestamp,
		.ttl = 0,
		.rank = rank,
		.count = rr->rrs.rr_count
	};
	knot_rdata_t *rd = rr->rrs.data;
	for (uint16_t i = 0; i < rr->rrs.rr_count; ++i) {
		if (knot_rdata_ttl(rd) > header.ttl) {
			header.ttl = knot_rdata_ttl(rd);
		}
		rd = kr_rdataset_next(rd);
	}

	namedb_val_t data = { rr->rrs.data, knot_rdataset_size(&rr->rrs) };
	return kr_cache_insert(txn, KR_CACHE_RR, rr->owner, rr->type, &header, data);
}

int kr_cache_peek_rrsig(struct kr_cache_txn *txn, knot_rrset_t *rr, uint16_t *rank, uint32_t *timestamp)
{
	if (!txn_is_valid(txn) || !rr || !timestamp) {
		return kr_error(EINVAL);
	}

	/* Check if the RRSet is in the cache. */
	struct kr_cache_entry *entry = NULL;
	int ret = kr_cache_peek(txn, KR_CACHE_SIG, rr->owner, rr->type, &entry, timestamp);
	if (ret != 0) {
		return ret;
	}
	if (rank) {
		*rank = entry->rank;
	}
	rr->type = KNOT_RRTYPE_RRSIG;
	rr->rrs.rr_count = entry->count;
	rr->rrs.data = entry->data;
	return kr_ok();
}

int kr_cache_insert_rrsig(struct kr_cache_txn *txn, const knot_rrset_t *rr, uint16_t rank, uint32_t timestamp)
{
	if (!txn_is_valid(txn) || !rr) {
		return kr_error(EINVAL);
	}

	/* Ignore empty records */
	if (knot_rrset_empty(rr)) {
		return kr_ok();
	}

	/* Prepare header to write */
	struct kr_cache_entry header = {
		.timestamp = timestamp,
		.ttl = 0,
		.rank = rank,
		.count = rr->rrs.rr_count
	};
	for (uint16_t i = 0; i < rr->rrs.rr_count; ++i) {
		knot_rdata_t *rd = knot_rdataset_at(&rr->rrs, i);
		if (knot_rdata_ttl(rd) > header.ttl) {
			header.ttl = knot_rdata_ttl(rd);
		}
	}

	uint16_t covered = knot_rrsig_type_covered(&rr->rrs, 0);
	namedb_val_t data = { rr->rrs.data, knot_rdataset_size(&rr->rrs) };
	return kr_cache_insert(txn, KR_CACHE_SIG, rr->owner, covered, &header, data);
}
