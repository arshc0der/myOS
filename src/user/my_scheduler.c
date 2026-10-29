/*
 * my_scheduler.c - template for YOUR scheduling policy.
 * Select it in the shell with:  sched mine
 * It is currently a simple FIFO queue; change pick() to experiment
 * (shortest job, lottery, ...). Tasks carry cpu_ticks, priority, slice_left.
 */
#include "sched.h"
#include "list.h"

static LIST_HEAD(my_queue);

static void my_enqueue(struct task *t) { list_add_tail(&t->queue_node, &my_queue); }

static void my_dequeue(struct task *t)
{
    if (!list_empty(&t->queue_node))
        list_del(&t->queue_node);
}

static struct task *my_pick(void)
{
    if (list_empty(&my_queue))
        return NULL;
    struct task *t = list_entry(my_queue.next, struct task, queue_node);
    list_del(&t->queue_node);
    return t;
}

static bool my_tick(struct task *running)
{
    if (running->slice_left)
        running->slice_left--;
    return running->slice_left == 0 && !list_empty(&my_queue);
}

const struct sched_policy sched_mine = {
    .name = "mine",
    .enqueue = my_enqueue,
    .dequeue = my_dequeue,
    .pick = my_pick,
    .tick = my_tick,
};
