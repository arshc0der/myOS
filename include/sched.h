/*
 * sched.h - tasks (threads) and the scheduler.
 *
 * A "task" is one flow of execution with its own stack. The scheduler
 * decides which task runs. The timer interrupt takes the CPU away from a
 * task after its time slice (preemption), so tasks cannot hog the CPU.
 *
 *     pid_t id = task_create("worker", my_function, NULL);
 *
 * The *policy* (which task runs next) is pluggable: see struct sched_policy
 * and templates/my_scheduler.c to write your own.
 */
#ifndef SCHED_H
#define SCHED_H
#include "types.h"
#include "list.h"
#include "sync.h"

typedef int pid_t;

#define TASK_NAME_MAX    24
#define TASK_STACK_SIZE  16384
#define TASK_MAX_FDS     16
#define TASK_CWD_MAX     128
#define TASK_SLICE_TICKS 5              /* 50 ms */
#define PRIO_LEVELS      4              /* 0 = highest, 3 = lowest */
#define PRIO_DEFAULT     2

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,      /* waiting for a time                */
    TASK_BLOCKED,       /* waiting on a wait queue           */
    TASK_ZOMBIE,        /* finished, waiting to be cleaned up */
};

#define TASK_JOINABLE 0x1   /* someone will call task_join() */

struct file;

struct task {
    pid_t            pid;
    char             name[TASK_NAME_MAX];
    enum task_state  state;
    uint32_t         esp;               /* saved stack pointer          */
    uint8_t         *stack;
    void           (*entry)(void *);
    void            *arg;
    int              priority;
    uint32_t         flags;
    uint32_t         wake_tick;
    int              exit_code;
    uint32_t         slice_left;
    uint32_t         cpu_ticks;         /* ticks spent running          */
    uint32_t         switches;          /* times scheduled in           */
    uint32_t         start_tick;
    struct list_head queue_node;        /* ready/wait queue membership  */
    struct list_head all_node;          /* list of every task           */
    struct wait_queue exit_wq;          /* joiners wait here            */
    struct file     *fds[TASK_MAX_FDS];
    char             cwd[TASK_CWD_MAX];
};

/* ---- scheduling policy (replace to experiment) ---- */
struct sched_policy {
    const char *name;
    void (*enqueue)(struct task *t);        /* t became ready            */
    void (*dequeue)(struct task *t);        /* remove t if it is queued  */
    struct task *(*pick)(void);             /* choose + remove next, or NULL */
    bool (*tick)(struct task *running);     /* every tick; true = preempt */
};
extern const struct sched_policy sched_round_robin;
extern const struct sched_policy sched_priority;
void sched_set_policy(const struct sched_policy *p);
const struct sched_policy *sched_get_policy(void);

/* ---- core ---- */
void  sched_init(void);                     /* the boot code becomes task 0 (idle) */
void  sched_start(void);                    /* turn on preemption */
bool  sched_running(void);
void  schedule(void);                       /* pick another task now */
struct task *task_current(void);
struct task *task_find(pid_t pid);

pid_t task_create(const char *name, void (*entry)(void *), void *arg);
pid_t task_create_ex(const char *name, void (*entry)(void *), void *arg,
                     int priority, uint32_t flags);
void  task_yield(void);
NORETURN void task_exit(int code);
int   task_kill(pid_t pid);                 /* 0 or -ESRCH / -EPERM */
int   task_join(pid_t pid, int *exit_code); /* wait for TASK_JOINABLE task */
int   task_set_priority(pid_t pid, int prio);

void  sched_sleep_until(uint32_t tick);
void  sched_block_current(void);            /* irq must be off */
void  sched_make_ready(struct task *t);

/* Preemption control: while count > 0 the timer will not switch tasks. */
void  preempt_disable(void);
void  preempt_enable(void);
bool  sched_need_resched(void);
void  sched_preempt(void);                  /* called from the timer IRQ */

void  sched_reap(void);                     /* free finished tasks */
void  sched_for_each(void (*cb)(struct task *, void *), void *ctx);
uint32_t sched_task_count(void);
uint32_t sched_total_switches(void);

#endif
