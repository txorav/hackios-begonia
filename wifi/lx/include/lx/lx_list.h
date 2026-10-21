/*
 * lx_list.h — minimal doubly-linked list + hash list for the lx shim layer.
 *
 * This is a clean-room reimplementation of the classic intrusive list
 * container (mirrors include/linux/list.h patterns found in every Linux
 * driver we port; interface names preserved for readability).
 *
 * Constraints honored:
 *  - Freestanding: no libc, no kernel headers — header-only.
 *  - Same-ELEMENT semantics: entries live inside the containing object
 *    (no allocations), which is exactly what the ported drivers need for
 *    block request queues (msdc1/ufs0) and touch packet ring buffers.
 *  - Not ISR-safe by itself: callers wrap with lx_spinlock ops when the
 *    list is touched from primary interrupt context (see lx_spinlock.h).
 */

#ifndef LX_LIST_H
#define LX_LIST_H

#include "lx_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* ----------------------------------------------------------------------
 * struct list_head — standard intrusive doubly-linked list.
 * ---------------------------------------------------------------------- */
struct lx_list_head {
	struct lx_list_head *prev;
	struct lx_list_head *next;
};

#define LX_LIST_HEAD_INIT(name) { &(name), &(name) }
#define LX_LIST_HEAD(name) \
	struct lx_list_head name = LX_LIST_HEAD_INIT(name)

static inline void lx_list_init(struct lx_list_head *head)
{
	head->next = head;
	head->prev = head;
}

/* Insert `entry` right after `head` (LIFO/FIFO choice of caller). */
static inline void lx_list_add(struct lx_list_head *entry,
    struct lx_list_head *head)
{
	entry->prev = head;
	entry->next = head->next;
	head->next->prev = entry;
	head->next = entry;
}

/* Insert `entry` just before `head` (i.e., at tail). */
static inline void lx_list_add_tail(struct lx_list_head *entry,
    struct lx_list_head *head)
{
	entry->prev = head->prev;
	entry->next = head;
	head->prev->next = entry;
	head->prev = entry;
}

/* Unlink (no-op safe: entry not in a list is a no-op — unlike Linux where
 * a corrupted list spins; we choose safety over speed for bring-up). */
static inline void lx_list_del(struct lx_list_head *entry)
{
	entry->next->prev = entry->prev;
	entry->prev->next = entry->next;
	entry->prev = entry;
	entry->next = entry;
}

static inline int lx_list_empty(const struct lx_list_head *head)
{
	return head->next == head;
}

/* container_of equivalent */
#define lx_list_entry(ptr, type, member) \
	((type *)((char *)(ptr) - (unsigned long)&((type *)0)->member))

/* Iterate forward */
#define lx_list_for_each(iter, head) \
	for ((iter) = (head)->next; (iter) != (head); (iter) = (iter)->next)

/* Iterate forward, allowing iter removal (save next) */
#define lx_list_for_each_safe(iter, tmp, head) \
	for ((iter) = (head)->next, (tmp) = (iter)->next; \
	    (iter) != (head); \
	    (iter) = (tmp), (tmp) = (iter)->next)

/* Iterate entries of a given type */
#define lx_list_for_each_entry(iter, head, type, member) \
	for ((iter) = lx_list_entry((head)->next, type, member); \
	    &(iter)->member != (head); \
	    (iter) = lx_list_entry((iter)->member.next, type, member))

/* Iterate entries with a safe `next` pointer (removal from inside ok).
 * NOTE: items being REMOVED in the body are unlinked with lx_list_del()
 * (which restores self-links, making removal idempotent). */
#define lx_list_for_each_entry_safe(iter, tmp, head, type, member) \
	for ((iter) = lx_list_entry((head)->next, type, member), \
	    (tmp) = ((&((iter)->member) != (head)) \
	        ? lx_list_entry((iter)->member.next, type, member) : (iter)); \
	    (&((iter)->member) != (head)); \
	    (iter) = (tmp), \
	    (tmp) = ((&((iter)->member) != (head)) \
	        ? lx_list_entry((iter)->member.next, type, member) : (iter)))

/* Reverse iteration (for msdc logic that drains a queue from the oldest) */
#define lx_list_for_each_entry_reverse(iter, head, type, member) \
	for ((iter) = lx_list_entry((head)->prev, type, member); \
	    &(iter)->member != (head); \
	    (iter) = lx_list_entry((iter)->member.prev, type, member))

/* ----------------------------------------------------------------------
 * struct lx_hlist_head — singly-linked "hash bucket" list.
 * ---------------------------------------------------------------------- */
struct lx_hlist_node {
	struct lx_hlist_node *next;
};

struct lx_hlist_head {
	struct lx_hlist_node *first;
};

#define LX_HLIST_HEAD_INIT {.first = NULL}
#define LX_HLIST_HEAD(name) \
	struct lx_hlist_head name = { .first = NULL }

static inline void lx_hlist_add_head(struct lx_hlist_node *entry,
    struct lx_hlist_head *head)
{
	entry->next = head->first;
	head->first = entry;
}

static inline void lx_hlist_del(struct lx_hlist_node *entry,
    struct lx_hlist_head *head)
{
	struct lx_hlist_node *it = head->first;
	struct lx_hlist_node *prev = NULL;
	while (it != NULL && it != entry) {
		prev = it;
		it = it->next;
	}
	if (it == NULL)
		return;                 /* not present: no-op (safe branch) */
	if (prev == NULL)
		head->first = entry->next;
	else
		prev->next = entry->next;
}

#define lx_hlist_for_each(iter, head) \
	for ((iter) = (head)->first; (iter) != NULL; (iter) = (iter)->next)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_LIST_H */
