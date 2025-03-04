/* key-chain for authentication.
 * Copyright (C) 2000 Kunihiro Ishiguro
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _ZEBRA_KEYCHAIN_H
#define _ZEBRA_KEYCHAIN_H

#include "qobj.h"

#ifdef __cplusplus
extern "C" {
#endif

enum keychain_hash_algo {
	KEYCHAIN_ALGO_NULL,
	KEYCHAIN_ALGO_MD5,
	KEYCHAIN_ALGO_HMAC_SHA1,
	KEYCHAIN_ALGO_HMAC_SHA256,
	KEYCHAIN_ALGO_HMAC_SHA384,
	KEYCHAIN_ALGO_HMAC_SHA512,
	KEYCHAIN_ALGO_MAX
};

#define KEYCHAIN_MD5_HASH_SIZE 16
#define KEYCHAIN_HMAC_SHA1_HASH_SIZE 20
#define KEYCHAIN_HMAC_SHA256_HASH_SIZE 32
#define KEYCHAIN_HMAC_SHA384_HASH_SIZE 48
#define KEYCHAIN_HMAC_SHA512_HASH_SIZE 64
#define KEYCHAIN_MAX_HASH_SIZE 64

#define KEYCHAIN_ALGO_MD5_INTERNAL_BLK_SIZE 16
#define KEYCHAIN_ALGO_SHA1_INTERNAL_BLK_SIZE 64
#define KEYCHAIN_ALGO_SHA256_INTERNAL_BLK_SIZE 64
#define KEYCHAIN_ALGO_SHA384_INTERNAL_BLK_SIZE 128
#define KEYCHAIN_ALGO_SHA512_INTERNAL_BLK_SIZE 128
#define KEYCHAIN_ALGO_MAX_INTERNAL_BLK_SIZE 128

struct keychain_algo_info {
	enum keychain_hash_algo key;
	const char *name;
	uint32_t length;
	uint32_t block;
	const char *desc;
};

extern const struct keychain_algo_info algo_info[];
uint32_t keychain_get_block_size(enum keychain_hash_algo key);
uint32_t keychain_get_hash_len(enum keychain_hash_algo key);
const char *keychain_get_description(enum keychain_hash_algo key);
struct keychain_algo_info
keychain_get_hash_algo_info(enum keychain_hash_algo key);
enum keychain_hash_algo keychain_get_algo_id_by_name(const char *name);
const char *keychain_get_algo_name_by_id(enum keychain_hash_algo key);

struct keychain {
	char *name;

	struct list *key;

	QOBJ_FIELDS;
};
DECLARE_QOBJ_TYPE(keychain);

struct key_range {
	time_t start;
	time_t end;

	uint8_t duration;
};

struct key {
	uint32_t index;

	char *string;
	enum keychain_hash_algo hash_algo;
	struct key_range send;
	struct key_range accept;

	QOBJ_FIELDS;
};
DECLARE_QOBJ_TYPE(key);

extern void keychain_init(void);
extern struct keychain *keychain_lookup(const char *);
extern struct key *key_lookup_for_accept(const struct keychain *, uint32_t);
extern struct key *key_match_for_accept(const struct keychain *, const char *);
extern struct key *key_lookup_for_send(const struct keychain *);
const char *keychain_algo_str(enum keychain_hash_algo hash_algo);
#ifdef __cplusplus
}
#endif

#endif /* _ZEBRA_KEYCHAIN_H */
