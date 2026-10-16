/*
 * iav_arena.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#include "iav_arena.h"

static iav_arena_u64 arena_round_up(iav_arena_u64 v, iav_arena_u64 a, int *overflow)
{
	iav_arena_u64 r = v + (a - 1);

	if (r < v) {
		*overflow = 1;
		return 0;
	}
	return r & ~(a - 1);
}

int iav_arena_init(struct iav_arena *a, iav_arena_u64 usable, iav_arena_u64 page, void *(*zalloc)(size_t),
		   void (*free_fn)(void *))
{
	if (!a || usable == 0 || page == 0 || (page & (page - 1)) || page > IAV_ARENA_ALIGN || !zalloc || !free_fn)
		return -EINVAL;
	memset(a, 0, sizeof(*a));
	a->usable = usable;
	a->page = page;
	a->next_n = 1;
	a->zalloc = zalloc;
	a->free = free_fn;
	return 0;
}

void iav_arena_destroy(struct iav_arena *a)
{
	struct iav_arena_blk *b, *n;

	if (!a)
		return;
	for (b = a->head; b; b = n) {
		n = b->next;
		a->free(b);
	}
	a->head = NULL;
	a->nblk = 0;
	a->used = 0;
}

struct iav_arena_blk *iav_arena_find(struct iav_arena *a, iav_arena_u32 vid)
{
	struct iav_arena_blk *b;

	if (!a || !iav_arena_is_anon(vid))
		return NULL;
	for (b = a->head; b; b = b->next)
		if (b->vid == vid)
			return b;
	return NULL;
}

struct iav_arena_blk *iav_arena_find_at(struct iav_arena *a, iav_arena_u64 off)
{
	struct iav_arena_blk *b;

	if (!a)
		return NULL;
	for (b = a->head; b; b = b->next)
		if (b->off == off)
			return b;
	return NULL;
}

/* The next id not in use. 0 when every id is in use (2^24 - 1 blocks cannot fit; kept for safety). */
static iav_arena_u32 next_vid(struct iav_arena *a)
{
	iav_arena_u32 tries;

	for (tries = 0; tries < IAV_ARENA_MAX_N; tries++) {
		iav_arena_u32 n = a->next_n;
		iav_arena_u32 vid = (n << 8) | IAV_ARENA_ANON_TAG;

		a->next_n = n >= IAV_ARENA_MAX_N ? 1 : n + 1;
		if (!iav_arena_find(a, vid))
			return vid;
	}
	return 0;
}

static struct iav_arena_user *user_of(struct iav_arena_blk *b, const void *file)
{
	iav_arena_u32 i;

	for (i = 0; i < b->nusers; i++)
		if (b->users[i].file == file)
			return &b->users[i];
	return NULL;
}

static int add_user(struct iav_arena_blk *b, const void *file)
{
	if (user_of(b, file))
		return 0;
	if (b->nusers >= IAV_ARENA_MAX_USERS)
		return -EINVAL;
	b->users[b->nusers].file = file;
	b->users[b->nusers].maps = 0;
	b->nusers++;
	return 0;
}

static void drop_user(struct iav_arena_blk *b, const void *file)
{
	iav_arena_u32 i;

	for (i = 0; i < b->nusers; i++) {
		if (b->users[i].file == file) {
			b->users[i] = b->users[b->nusers - 1];
			b->nusers--;
			return;
		}
	}
}

static void unlink_blk(struct iav_arena *a, struct iav_arena_blk *blk)
{
	struct iav_arena_blk **pp;

	for (pp = &a->head; *pp; pp = &(*pp)->next) {
		if (*pp == blk) {
			*pp = blk->next;
			a->nblk--;
			a->used -= blk->span;
			a->free(blk);
			return;
		}
	}
}

int iav_arena_alloc(struct iav_arena *a, const void *file, iav_arena_u64 req, iav_arena_u32 *vid,
		    iav_arena_u64 *off, iav_arena_u64 *len)
{
	struct iav_arena_blk *b, *prev = NULL, *nb;
	iav_arena_u64 l, span, at = 0;
	int overflow = 0;

	if (!a || !vid || !off || !len)
		return -EINVAL;
	if (req == 0)
		return -EINVAL;
	l = arena_round_up(req, a->page, &overflow);
	span = arena_round_up(l, IAV_ARENA_ALIGN, &overflow);
	if (overflow || span > a->usable)
		return -EFAULT;
	/* first fit: the lowest gap between blocks (in offset order) that holds the span */
	for (b = a->head; b; prev = b, b = b->next) {
		if (b->off >= at && b->off - at >= span)
			break;
		at = b->off + b->span;
	}
	if (at > a->usable || a->usable - at < span)
		return -EFAULT;
	nb = a->zalloc(sizeof(*nb));
	if (!nb)
		return -EFAULT;
	nb->vid = next_vid(a);
	if (nb->vid == 0) {
		a->free(nb);
		return -EFAULT;
	}
	nb->off = at;
	nb->len = l;
	nb->span = span;
	nb->users[0].file = file;
	nb->nusers = 1;
	nb->next = b;
	if (prev)
		prev->next = nb;
	else
		a->head = nb;
	a->nblk++;
	a->used += span;
	*vid = nb->vid;
	*off = nb->off;
	*len = nb->len;
	return 0;
}

int iav_arena_free(struct iav_arena *a, iav_arena_u32 vid)
{
	struct iav_arena_blk *b = iav_arena_find(a, vid);

	if (!b)
		return -EINVAL;
	if (b->maps)
		return -EFAULT;
	unlink_blk(a, b);
	return 0;
}

int iav_arena_register(struct iav_arena *a, iav_arena_u32 vid, const void *file)
{
	struct iav_arena_blk *b = iav_arena_find(a, vid);

	if (!b)
		return -EINVAL;
	return add_user(b, file);
}

int iav_arena_unregister(struct iav_arena *a, iav_arena_u32 vid, const void *file)
{
	struct iav_arena_blk *b = iav_arena_find(a, vid);
	struct iav_arena_user *u;

	if (!b)
		return -EINVAL;
	u = user_of(b, file);
	if (u && u->maps)
		return -EPERM;
	drop_user(b, file);
	return 0;
}

int iav_arena_map(struct iav_arena *a, struct iav_arena_blk *blk, const void *file)
{
	struct iav_arena_user *u;

	(void)a;
	if (!blk || add_user(blk, file) != 0)
		return -EINVAL;
	u = user_of(blk, file);
	u->maps++;
	blk->maps++;
	return 0;
}

void iav_arena_unmap(struct iav_arena *a, struct iav_arena_blk *blk, const void *file)
{
	struct iav_arena_user *u;

	(void)a;
	if (!blk)
		return;
	u = user_of(blk, file);
	if (u && u->maps)
		u->maps--;
	if (blk->maps)
		blk->maps--;
}

void iav_arena_release_file(struct iav_arena *a, const void *file)
{
	struct iav_arena_blk *b, *n;

	if (!a)
		return;
	for (b = a->head; b; b = b->next)
		drop_user(b, file);
	for (b = a->head; b; b = n) {
		n = b->next;
		if (b->nusers == 0 && b->maps == 0)
			unlink_blk(a, b);
	}
}
