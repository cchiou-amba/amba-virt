/*
 * iav_arena.h
 *
 * The guest side of the per-guest arena (plan_iav_usb_yolo, Envelope E): a first-fit allocator over the guest's
 * private IAV slice that answers the bare-metal anonymous-partition calls (ALLOC_ANON_MEM_PART, FREE_MEM_PART,
 * REGISTER_ANON_MEM_PART, UNREGISTER_ANON_MEM_PART, QUERY_MEMBLOCK) with virtual anonymous ids in the bare-metal
 * format ((n << 8) | 0xff). It follows the bare-metal driver's rules (ambcma/ambcma_drv.c): a block has a list of
 * users (the descriptor that allocated it, those that registered it and those that mapped it); FREE refuses a mapped
 * block (EFAULT), UNREGISTER refuses a descriptor that still maps it (EPERM), and when a descriptor is released every
 * block left with no user and no mapping is freed. Blocks start on a 64 KiB boundary (a 64 KiB-page guest can map
 * every block) and report their length rounded to the guest page, as the bare-metal driver reports the page-rounded
 * size.
 *
 * Pure C with no kernel dependency, so the same file is unit-tested on the build host. The caller serializes every
 * call (one lock around the arena). Memory for the bookkeeping comes from the zalloc/free the caller gives.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef IAV_ARENA_H
#define IAV_ARENA_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 iav_arena_u32;
typedef u64 iav_arena_u64;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint32_t iav_arena_u32;
typedef uint64_t iav_arena_u64;
#endif

#define IAV_ARENA_ALIGN		0x10000ull	/* AMBA_VIRT_IAV_ARENA_ALIGN */
#define IAV_ARENA_ANON_TAG	0xffu
#define IAV_ARENA_MAX_N		0xffffffu	/* ids (n << 8) | 0xff with n in [1, IAV_ARENA_MAX_N] */
#define IAV_ARENA_MAX_USERS	64u		/* descriptors on one block */

struct iav_arena_user {
	const void *file;
	iav_arena_u32 maps;
};

struct iav_arena_blk {
	struct iav_arena_blk *next;	/* in offset order */
	iav_arena_u32 vid;
	iav_arena_u64 off;		/* from the start of the slice, a multiple of IAV_ARENA_ALIGN */
	iav_arena_u64 len;		/* reported: the request rounded to the page */
	iav_arena_u64 span;		/* reserved: len rounded to IAV_ARENA_ALIGN */
	iav_arena_u32 maps;		/* mappings of the block, all descriptors */
	iav_arena_u32 nusers;
	struct iav_arena_user users[IAV_ARENA_MAX_USERS];
};

struct iav_arena {
	iav_arena_u64 usable;		/* bytes of the slice the arena may use */
	iav_arena_u64 page;		/* the guest page size */
	iav_arena_u32 next_n;
	iav_arena_u32 nblk;
	iav_arena_u64 used;		/* sum of the spans */
	struct iav_arena_blk *head;
	void *(*zalloc)(size_t bytes);
	void (*free)(void *p);
};

static inline int iav_arena_is_anon(iav_arena_u32 id)
{
	return (id & 0xffu) == IAV_ARENA_ANON_TAG && (id >> 8) != 0;
}

/* usable and page must be non-zero; page a power of two not larger than IAV_ARENA_ALIGN. 0 or -EINVAL. */
int iav_arena_init(struct iav_arena *a, iav_arena_u64 usable, iav_arena_u64 page, void *(*zalloc)(size_t),
		   void (*free_fn)(void *));
/* Free every block, whatever its users. */
void iav_arena_destroy(struct iav_arena *a);

/*
 * ALLOC_ANON_MEM_PART: a block of req bytes for file. 0 with *vid, *off (from the slice start) and *len (reported), or
 * -EINVAL for a zero length, -EFAULT when it does not fit (the bare-metal answer to a failed allocation).
 */
int iav_arena_alloc(struct iav_arena *a, const void *file, iav_arena_u64 req, iav_arena_u32 *vid,
		    iav_arena_u64 *off, iav_arena_u64 *len);
/* The live block with this id, or NULL. */
struct iav_arena_blk *iav_arena_find(struct iav_arena *a, iav_arena_u32 vid);
/* The live block that starts at off, or NULL. */
struct iav_arena_blk *iav_arena_find_at(struct iav_arena *a, iav_arena_u64 off);
/* FREE_MEM_PART: -EINVAL unknown, -EFAULT while mapped, else 0. */
int iav_arena_free(struct iav_arena *a, iav_arena_u32 vid);
/* REGISTER_ANON_MEM_PART: -EINVAL unknown or too many users, else 0 (registering twice is one user). */
int iav_arena_register(struct iav_arena *a, iav_arena_u32 vid, const void *file);
/* UNREGISTER_ANON_MEM_PART: -EINVAL unknown, -EPERM while this file maps it, else 0. The block is not freed. */
int iav_arena_unregister(struct iav_arena *a, iav_arena_u32 vid, const void *file);
/* A mapping of blk by file was made (mmap, or a copy of the VMA): -EINVAL if the user table is full. */
int iav_arena_map(struct iav_arena *a, struct iav_arena_blk *blk, const void *file);
/* That mapping is gone. */
void iav_arena_unmap(struct iav_arena *a, struct iav_arena_blk *blk, const void *file);
/* file is released: it leaves every block, and every block left with no user and no mapping is freed. */
void iav_arena_release_file(struct iav_arena *a, const void *file);

#endif /* IAV_ARENA_H */
