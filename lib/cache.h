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

#pragma once

#include <libknot/rrset.h>
#include <libknot/db/db.h>
#include "lib/defines.h"

/** Cache entry tag */
enum kr_cache_tag {
	KR_CACHE_RR   = 'R',
	KR_CACHE_PKT  = 'P',
	KR_CACHE_SIG  = 'G',
	KR_CACHE_USER = 0x80
};

/**
 * Cache entry rank.
 * @note Be careful about chosen cache rank nominal values.
 * - AUTH must be > than NONAUTH
 * - AUTH INSECURE must be > than AUTH (because it attempted validation)
 * - NONAUTH SECURE must be > than AUTH (because it's valid)
 */
enum kr_cache_rank {
	KR_RANK_BAD       = 0,  /* BAD cache, do not use. */ 
	KR_RANK_INSECURE  = 1,  /* Entry is DNSSEC insecure (e.g. RRSIG not exists). */
	KR_RANK_NONAUTH   = 8,  /* Entry from authority section (i.e. parent-side) */
	KR_RANK_AUTH      = 16, /* Entry from answer (authoritative data) */
	KR_RANK_SECURE    = 64, /* Entry is DNSSEC valid (e.g. RRSIG exists). */
	/* @note Rank must not exceed 6 bits */
};

/** Cache entry flags */
enum kr_cache_flag {
	KR_CACHE_FLAG_NONE	  = 0,
	KR_CACHE_FLAG_WCARD_PROOF = 1  /* Entry contains either packet with wildcard
                                        * answer either record for which wildcard
                                        * expansion proof is needed */
};


/**
 * Serialized form of the RRSet with inception timestamp and maximum TTL.
 */
struct kr_cache_entry
{
	uint32_t timestamp;
	uint32_t ttl;
	uint16_t count;
	uint8_t  rank;
	uint8_t  flags;
	uint8_t  data[];
};

/**
 * Cache structure, keeps API, instance and metadata.
 */
struct kr_cache
{
	knot_db_t *db;		      /**< Storage instance */
	const knot_db_api_t *api;      /**< Storage engine */
	struct {
		uint32_t hit;         /**< Number of cache hits */
		uint32_t miss;        /**< Number of cache misses */
		uint32_t insert;      /**< Number of insertions */
		uint32_t delete;      /**< Number of deletions */
		uint32_t txn_read;    /**< Number of read transactions */
		uint32_t txn_write;   /**< Number of write transactions */
	} stats;
};

/** Cache transaction */
struct kr_cache_txn {
    knot_db_txn_t t;          /**< Storage transaction */  
    struct kr_cache *owner;  /**< Transaction owner */
};

/**
 * Open/create cache with provided storage options.
 * @param cache cache structure to be initialized
 * @param api   storage engine API
 * @param opts  storage-specific options (may be NULL for default)
 * @param mm    memory context.
 * @return 0 or an error code
 */
KR_EXPORT
int kr_cache_open(struct kr_cache *cache, const knot_db_api_t *api, void *opts, knot_mm_t *mm);

/**
 * Close persistent cache.
 * @note This doesn't clear the data, just closes the connection to the database.
 * @param cache database instance
 */
KR_EXPORT
void kr_cache_close(struct kr_cache *cache);

/**
 * Begin cache transaction (read-only or write).
 *
 * @param cache database instance
 * @param txn transaction instance to be initialized (output)
 * @param flags transaction flags (see namedb.h in libknot)
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_txn_begin(struct kr_cache *cache, struct kr_cache_txn *txn, unsigned flags);

/**
 * Commit existing transaction.
 * @param txn transaction instance
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_txn_commit(struct kr_cache_txn *txn);

/**
 * Abort existing transaction instance.
 * @param txn transaction instance
 */
KR_EXPORT
void kr_cache_txn_abort(struct kr_cache_txn *txn);

/**
 * Peek the cache for asset (name, type, tag)
 * @note The 'drift' is the time passed between the inception time and now (in seconds).
 * @param txn transaction instance
 * @param tag  asset tag
 * @param name asset name
 * @param type asset type
 * @param entry cache entry, will be set to valid pointer or NULL
 * @param timestamp current time (will be replaced with drift if successful)
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_peek(struct kr_cache_txn *txn, uint8_t tag, const knot_dname_t *name, uint16_t type,
                  struct kr_cache_entry **entry, uint32_t *timestamp);



/**
 * Insert asset into cache, replacing any existing data.
 * @param txn transaction instance
 * @param tag  asset tag
 * @param name asset name
 * @param type asset type
 * @param header filled entry header (count, ttl and timestamp)
 * @param data inserted data
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_insert(struct kr_cache_txn *txn, uint8_t tag, const knot_dname_t *name, uint16_t type,
                    struct kr_cache_entry *header, knot_db_val_t data);

/**
 * Remove asset from cache.
 * @param txn transaction instance
 * @param tag asset tag
 * @param name asset name
 * @param type record type
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_remove(struct kr_cache_txn *txn, uint8_t tag, const knot_dname_t *name, uint16_t type);

/**
 * Clear all items from the cache.
 * @param txn transaction instance
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_clear(struct kr_cache_txn *txn);

/**
 * Peek the cache for given key and retrieve it's rank.
 * @param txn transaction instance
 * @param tag asset tag
 * @param name asset name
 * @param type record type
 * @param timestamp current time
 * @return rank (0 or positive), or an error (negative number)
 */
KR_EXPORT
int kr_cache_peek_rank(struct kr_cache_txn *txn, uint8_t tag, const knot_dname_t *name, uint16_t type, uint32_t timestamp);

/**
 * Peek the cache for given RRSet (name, type)
 * @note The 'drift' is the time passed between the cache time of the RRSet and now (in seconds).
 * @param txn transaction instance
 * @param rr query RRSet (its rdataset may be changed depending on the result)
 * @param rank entry rank will be stored in this variable
 * @param flags entry flags
 * @param timestamp current time (will be replaced with drift if successful)
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_peek_rr(struct kr_cache_txn *txn, knot_rrset_t *rr, uint8_t *rank, uint8_t *flags, uint32_t *timestamp);

/**
 * Clone read-only RRSet and adjust TTLs.
 * @param dst destination for materialized RRSet
 * @param src read-only RRSet (its rdataset may be changed depending on the result)
 * @param drift time passed between cache time and now
 * @param mm memory context
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_materialize(knot_rrset_t *dst, const knot_rrset_t *src, uint32_t drift, knot_mm_t *mm);

/**
 * Insert RRSet into cache, replacing any existing data.
 * @param txn transaction instance
 * @param rr inserted RRSet
 * @param rank rank of the data
 * @param flags additional flags for the data
 * @param timestamp current time
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_insert_rr(struct kr_cache_txn *txn, const knot_rrset_t *rr, uint8_t rank, uint8_t flags, uint32_t timestamp);

/**
 * Peek the cache for the given RRset signature (name, type)
 * @note The RRset type must not be RRSIG but instead it must equal the type covered field of the sought RRSIG.
 * @param txn transaction instance
 * @param rr query RRSET (its rdataset and type may be changed depending on the result)
 * @param rank entry rank will be stored in this variable
 * @param flags entry additional flags
 * @param timestamp current time (will be replaced with drift if successful)
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_peek_rrsig(struct kr_cache_txn *txn, knot_rrset_t *rr, uint8_t *rank, uint8_t *flags, uint32_t *timestamp);

/**
 * Insert the selected RRSIG RRSet of the selected type covered into cache, replacing any existing data.
 * @note The RRSet must contain RRSIGS with only the specified type covered.
 * @param txn transaction instance
 * @param rr inserted RRSIG RRSet
 * @param rank rank of the data
 * @param flags additional flags for the data
 * @param timestamp current time
 * @return 0 or an errcode
 */
KR_EXPORT
int kr_cache_insert_rrsig(struct kr_cache_txn *txn, const knot_rrset_t *rr, uint8_t rank, uint8_t flags, uint32_t timestamp);
