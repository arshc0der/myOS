/*
 * list.h - intrusive doubly linked list (the same idea as Linux's list.h).
 *
 * Put a `struct list_head` inside your own struct, then link those heads
 * together. list_entry() gets back to your struct.
 */
#ifndef LIST_H
#define LIST_H

#include "types.h"

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void list_init(struct list_head *h)
{
    h->next = h;
    h->prev = h;
}

static inline bool list_empty(const struct list_head *h)
{
    return h->next == h;
}

static inline void __list_add(struct list_head *n, struct list_head *prev,
                              struct list_head *next)
{
    next->prev = n;
    n->next = next;
    n->prev = prev;
    prev->next = n;
}

/* Insert at the front of the list. */
static inline void list_add(struct list_head *n, struct list_head *head)
{
    __list_add(n, head, head->next);
}

/* Insert at the end of the list. */
static inline void list_add_tail(struct list_head *n, struct list_head *head)
{
    __list_add(n, head->prev, head);
}

static inline void list_del(struct list_head *e)
{
    e->next->prev = e->prev;
    e->prev->next = e->next;
    e->next = e;
    e->prev = e;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/* Safe against removing `pos` while iterating. */
#define list_for_each_safe(pos, tmp, head) \
    for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head); \
         (pos) = (tmp), (tmp) = (pos)->next)

#endif
