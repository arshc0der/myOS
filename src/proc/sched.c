/*
 * sched.c - tasks and the scheduler core.
 *
 * Idea: every task has its own kernel stack. To switch tasks we save the
 * CPU registers of the old task on its stack and load the new task's
 * stack (switch_context in switch.S). The *policy* only decides WHICH task
 * is next; this file does the switching.
 */
#include "sched.h"
#include "heap.h"
#include "timer.h"
#include "kstring.h"
#include "kerror.h"
#include "io.h"
#include "log.h"
#include "panic.h"

extern void switch_context(uint32_t *save_old_esp, uint32_t new_esp);
extern void vfs_task_cleanup(struct task *t) __attribute__((weak));

static struct task idle_task;
static struct task *current = &idle_task;
static LIST_HEAD(all_tasks);
static const struct sched_policy *policy = &sched_round_robin;
static pid_t next_pid = 1;
static bool running;
static volatile bool need_resched;
static int preempt_count;
static uint32_t total_switches, task_count;

struct task *task_current(void) { return current; }
bool sched_running(void) { return running; }
uint32_t sched_task_count(void) { return task_count; }
uint32_t sched_total_switches(void) { return total_switches; }
const struct sched_policy *sched_get_policy(void) { return policy; }

void sched_init(void)
{
    memset(&idle_task, 0, sizeof idle_task);
    idle_task.pid = 0;
    strlcpy(idle_task.name, "idle", sizeof idle_task.name);
    idle_task.state = TASK_RUNNING;
    idle_task.priority = PRIO_LEVELS - 1;
    strlcpy(idle_task.cwd, "/", sizeof idle_task.cwd);
    list_init(&idle_task.queue_node);
    wq_init(&idle_task.exit_wq);
    list_add_tail(&idle_task.all_node, &all_tasks);
    task_count = 1;
}

void sched_make_ready(struct task *t)
{
    t->state = TASK_READY;
    policy->enqueue(t);
    if (current == &idle_task)
        need_resched = true;
}

/* ---- the switch itself ---- */

void schedule(void)
{
    uint32_t flags = irq_save();
    struct task *prev = current;

    if (prev->state == TASK_RUNNING) {          /* still wants the CPU */
        prev->state = TASK_READY;
        if (prev != &idle_task)
            policy->enqueue(prev);
    }

    struct task *next = policy->pick();
    if (!next)
        next = &idle_task;
    next->state = TASK_RUNNING;
    next->slice_left = TASK_SLICE_TICKS;

    if (next != prev) {
        current = next;
        next->switches++;
        total_switches++;
        switch_context(&prev->esp, next->esp);
        /* We are back: someone switched to us again. */
    }
    irq_restore(flags);
}

static void task_start(void)
{
    cpu_sti();                                  /* new tasks start with IRQs on */
    current->entry(current->arg);
    task_exit(0);
}

pid_t task_create_ex(const char *name, void (*entry)(void *), void *arg,
                     int priority, uint32_t flags)
{
    struct task *t = kzalloc(sizeof *t);
    if (!t)
        return -ENOMEM;
    t->stack = kmalloc(TASK_STACK_SIZE);
    if (!t->stack) {
        kfree(t);
        return -ENOMEM;
    }
    if (priority < 0) priority = 0;
    if (priority >= PRIO_LEVELS) priority = PRIO_LEVELS - 1;

    strlcpy(t->name, name, sizeof t->name);
    t->entry = entry;
    t->arg = arg;
    t->priority = priority;
    t->flags = flags;
    t->start_tick = timer_ticks();
    strlcpy(t->cwd, current->cwd, sizeof t->cwd);
    list_init(&t->queue_node);
    wq_init(&t->exit_wq);

    /* Build the first "saved context" so switch_context() can start us. */
    uint32_t *sp = (uint32_t *)(t->stack + TASK_STACK_SIZE);
    *--sp = 0;                                  /* fake return address  */
    *--sp = (uint32_t)task_start;               /* where `ret` goes     */
    *--sp = 0;                                  /* ebp */
    *--sp = 0;                                  /* ebx */
    *--sp = 0;                                  /* esi */
    *--sp = 0;                                  /* edi */
    t->esp = (uint32_t)sp;

    uint32_t f = irq_save();
    t->pid = next_pid++;
    list_add_tail(&t->all_node, &all_tasks);
    task_count++;
    sched_make_ready(t);
    irq_restore(f);
    return t->pid;
}

pid_t task_create(const char *name, void (*entry)(void *), void *arg)
{
    return task_create_ex(name, entry, arg, PRIO_DEFAULT, 0);
}

struct task *task_find(pid_t pid)
{
    struct list_head *p;
    list_for_each(p, &all_tasks) {
        struct task *t = list_entry(p, struct task, all_node);
        if (t->pid == pid && t->state != TASK_UNUSED)
            return t;
    }
    return NULL;
}

void task_yield(void)
{
    if (running)
        schedule();
}

NORETURN void task_exit(int code)
{
    if (current == &idle_task)
        PANIC("the idle task cannot exit");

    uint32_t f = irq_save();
    current->exit_code = code;
    current->state = TASK_ZOMBIE;
    if (vfs_task_cleanup)
        vfs_task_cleanup(current);
    wq_wake_all(&current->exit_wq);
    schedule();
    irq_restore(f);
    PANIC("zombie task was scheduled");
}

int task_kill(pid_t pid)
{
    struct task *t = task_find(pid);

    if (!t)
        return -ESRCH;
    if (t == &idle_task)
        return -EPERM;
    if (t == current)
        task_exit(-1);

    uint32_t f = irq_save();
    if (t->state == TASK_READY)
        policy->dequeue(t);
    else if (t->state == TASK_BLOCKED && !list_empty(&t->queue_node))
        list_del(&t->queue_node);
    t->exit_code = -1;
    t->state = TASK_ZOMBIE;
    if (vfs_task_cleanup)
        vfs_task_cleanup(t);
    wq_wake_all(&t->exit_wq);
    irq_restore(f);
    return 0;
}

static void free_task(struct task *t)
{
    list_del(&t->all_node);
    task_count--;
    kfree(t->stack);
    kfree(t);
}

void sched_reap(void)
{
    struct list_head *p, *n;
    uint32_t f = irq_save();

    list_for_each_safe(p, n, &all_tasks) {
        struct task *t = list_entry(p, struct task, all_node);
        if (t->state == TASK_ZOMBIE && !(t->flags & TASK_JOINABLE) &&
            t != current)
            free_task(t);
    }
    irq_restore(f);
}

int task_join(pid_t pid, int *exit_code)
{
    struct task *t = task_find(pid);

    if (!t)
        return -ESRCH;
    if (!(t->flags & TASK_JOINABLE) || t == current)
        return -EINVAL;
    WAIT_EVENT(t->exit_wq, t->state == TASK_ZOMBIE);
    if (exit_code)
        *exit_code = t->exit_code;
    uint32_t f = irq_save();
    free_task(t);
    irq_restore(f);
    return 0;
}

int task_set_priority(pid_t pid, int prio)
{
    struct task *t = task_find(pid);

    if (!t)
        return -ESRCH;
    if (prio < 0 || prio >= PRIO_LEVELS)
        return -EINVAL;
    uint32_t f = irq_save();
    if (t->state == TASK_READY) {
        policy->dequeue(t);
        t->priority = prio;
        policy->enqueue(t);
    } else {
        t->priority = prio;
    }
    irq_restore(f);
    return 0;
}

/* ---- sleeping and blocking ---- */

void sched_sleep_until(uint32_t tick)
{
    uint32_t f = irq_save();
    if (current == &idle_task)
        PANIC("the idle task cannot sleep");
    current->wake_tick = tick;
    current->state = TASK_SLEEPING;
    schedule();
    irq_restore(f);
}

void sched_block_current(void)
{
    if (current == &idle_task)
        PANIC("the idle task cannot block (run your code in a task)");
    current->state = TASK_BLOCKED;
    schedule();
}

/* ---- timer hook ---- */

static void sched_tick(void)
{
    uint32_t now = timer_ticks();
    struct list_head *p;

    current->cpu_ticks++;
    list_for_each(p, &all_tasks) {
        struct task *t = list_entry(p, struct task, all_node);
        if (t->state == TASK_SLEEPING && (int32_t)(now - t->wake_tick) >= 0)
            sched_make_ready(t);
    }
    if (current == &idle_task)
        need_resched = true;
    else if (policy->tick(current))
        need_resched = true;
}

void sched_start(void)
{
    timer_set_hook(sched_tick);
    running = true;
}

bool sched_need_resched(void) { return need_resched; }

void sched_preempt(void)
{
    if (running && need_resched && preempt_count == 0) {
        need_resched = false;
        schedule();
    }
}

void preempt_disable(void) { preempt_count++; }

void preempt_enable(void)
{
    if (preempt_count > 0)
        preempt_count--;
    if (preempt_count == 0 && need_resched && irq_enabled())
        sched_preempt();
}

/* ---- policy plumbing ---- */

void sched_set_policy(const struct sched_policy *p)
{
    uint32_t f = irq_save();
    struct list_head *it;

    list_for_each(it, &all_tasks) {
        struct task *t = list_entry(it, struct task, all_node);
        if (t->state == TASK_READY && t != &idle_task)
            policy->dequeue(t);
    }
    policy = p;
    list_for_each(it, &all_tasks) {
        struct task *t = list_entry(it, struct task, all_node);
        if (t->state == TASK_READY && t != &idle_task)
            policy->enqueue(t);
    }
    irq_restore(f);
}

void sched_for_each(void (*cb)(struct task *, void *), void *ctx)
{
    struct list_head *p;
    uint32_t f = irq_save();

    list_for_each(p, &all_tasks)
        cb(list_entry(p, struct task, all_node), ctx);
    irq_restore(f);
}

/* ---- built-in policies ---- */

static LIST_HEAD(rr_queue);

static void rr_enqueue(struct task *t) { list_add_tail(&t->queue_node, &rr_queue); }
static void rr_dequeue(struct task *t)
{
    if (!list_empty(&t->queue_node))
        list_del(&t->queue_node);
}
static struct task *rr_pick(void)
{
    if (list_empty(&rr_queue))
        return NULL;
    struct task *t = list_entry(rr_queue.next, struct task, queue_node);
    list_del(&t->queue_node);
    return t;
}
static bool rr_tick(struct task *r)
{
    if (r->slice_left)
        r->slice_left--;
    return r->slice_left == 0 && !list_empty(&rr_queue);
}

const struct sched_policy sched_round_robin = {
    "round-robin", rr_enqueue, rr_dequeue, rr_pick, rr_tick
};

static struct list_head prio_queue[PRIO_LEVELS] = {
    LIST_HEAD_INIT(prio_queue[0]), LIST_HEAD_INIT(prio_queue[1]),
    LIST_HEAD_INIT(prio_queue[2]), LIST_HEAD_INIT(prio_queue[3]),
};

static void pr_enqueue(struct task *t)
{
    list_add_tail(&t->queue_node, &prio_queue[t->priority]);
}
static void pr_dequeue(struct task *t)
{
    if (!list_empty(&t->queue_node))
        list_del(&t->queue_node);
}
static struct task *pr_pick(void)
{
    for (int i = 0; i < PRIO_LEVELS; i++) {
        if (!list_empty(&prio_queue[i])) {
            struct task *t = list_entry(prio_queue[i].next, struct task, queue_node);
            list_del(&t->queue_node);
            return t;
        }
    }
    return NULL;
}
static bool pr_tick(struct task *r)
{
    for (int i = 0; i < r->priority; i++)       /* a better task is waiting */
        if (!list_empty(&prio_queue[i]))
            return true;
    if (r->slice_left)
        r->slice_left--;
    if (r->slice_left == 0)
        for (int i = 0; i <= r->priority; i++)
            if (!list_empty(&prio_queue[i]))
                return true;
    return false;
}

const struct sched_policy sched_priority = {
    "priority", pr_enqueue, pr_dequeue, pr_pick, pr_tick
};
