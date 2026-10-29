/* cmd_proc.c - process (task) commands and demo programs. */
#include "shell.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"
#include "input.h"
#include "console.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerror.h"
#include "vfs.h"

static const char *state_name(enum task_state s)
{
    switch (s) {
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "running";
    case TASK_SLEEPING: return "sleeping";
    case TASK_BLOCKED:  return "blocked";
    case TASK_ZOMBIE:   return "zombie";
    default:            return "?";
    }
}

static void print_task(struct task *t, void *ctx)
{
    (void)ctx;
    kprintf("  %3d  %-14s %-9s %3d  %8u  %8u\n", t->pid, t->name,
            state_name(t->state), t->priority, t->cpu_ticks, t->switches);
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_set_color(CON_BRIGHT);
    kprintf("  PID  NAME           STATE     PRI  CPU-TICKS  SWITCHES\n");
    console_set_color(CON_NORMAL);
    sched_for_each(print_task, NULL);
    kprintf("%u tasks, scheduler: %s, total switches: %u\n", sched_task_count(),
            sched_get_policy()->name, sched_total_switches());
    return 0;
}
SHELL_COMMAND("ps", "ps", "List tasks", cmd_ps);

static int cmd_kill(int argc, char **argv)
{
    if (argc != 2) {
        kprintf("usage: kill <pid>\n");
        return 1;
    }
    int r = task_kill(katoi(argv[1]));
    if (r < 0) {
        kprintf("kill: %s\n", kstrerror(r));
        return 1;
    }
    return 0;
}
SHELL_COMMAND("kill", "kill <pid>", "Stop a task", cmd_kill);

static int cmd_nice(int argc, char **argv)
{
    if (argc != 3) {
        kprintf("usage: nice <pid> <priority 0-%d>   (0 = most important)\n", PRIO_LEVELS - 1);
        return 1;
    }
    int r = task_set_priority(katoi(argv[1]), katoi(argv[2]));
    if (r < 0) {
        kprintf("nice: %s\n", kstrerror(r));
        return 1;
    }
    return 0;
}
SHELL_COMMAND("nice", "nice <pid> <prio>", "Change a task's priority", cmd_nice);

static int cmd_sched(int argc, char **argv)
{
    if (argc == 1) {
        kprintf("scheduler policy: %s\navailable: round-robin, priority, mine\n",
                sched_get_policy()->name);
        return 0;
    }
    extern const struct sched_policy sched_mine;      /* src/user/my_scheduler.c */
    const struct sched_policy *p =
        strcmp(argv[1], "round-robin") == 0 ? &sched_round_robin :
        strcmp(argv[1], "priority") == 0 ? &sched_priority :
        strcmp(argv[1], "mine") == 0 ? &sched_mine : NULL;
    if (!p) {
        kprintf("unknown policy '%s'\n", argv[1]);
        return 1;
    }
    sched_set_policy(p);
    kprintf("scheduler policy is now: %s\n", p->name);
    return 0;
}
SHELL_COMMAND("sched", "sched [round-robin|priority|mine]", "Show or change the scheduling policy", cmd_sched);

/* ---------- demo programs: `spawn <name>` ---------- */

static void demo_counter(void *arg)
{
    int n = (int)(uint32_t)arg;

    for (int i = 1; i <= n; i++) {
        kprintf("[counter %d] %d of %d\n", task_current()->pid, i, n);
        timer_sleep_ms(1000);
    }
}

static void demo_hog(void *arg)
{
    (void)arg;
    volatile uint32_t x = 0;

    for (;;)
        x++;                                    /* burns CPU forever: use `kill` */
}

static struct semaphore items, spaces;
static struct mutex buf_lock = MUTEX_INIT(buf_lock);
static int ring[4], rhead, rtail;

static void producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= 8; i++) {
        sem_down(&spaces);
        mutex_lock(&buf_lock);
        ring[rhead] = i * 10;
        rhead = (rhead + 1) % 4;
        mutex_unlock(&buf_lock);
        kprintf("[producer] made %d\n", i * 10);
        sem_up(&items);
        timer_sleep_ms(150);
    }
}

static void consumer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= 8; i++) {
        sem_down(&items);
        mutex_lock(&buf_lock);
        int v = ring[rtail];
        rtail = (rtail + 1) % 4;
        mutex_unlock(&buf_lock);
        kprintf("[consumer] took %d\n", v);
        sem_up(&spaces);
        timer_sleep_ms(400);                    /* slower than the producer */
    }
}

static void demo_writer(void *arg)
{
    int id = (int)(uint32_t)arg;
    char name[40], line[40];

    ksnprintf(name, sizeof name, "/tmp/writer%d.log", id);
    for (int i = 0; i < 5; i++) {
        int n = ksnprintf(line, sizeof line, "writer %d: line %d\n", id, i);
        vfs_append_file(name, line, (size_t)n);
        timer_sleep_ms(100 + (uint32_t)id * 30);
    }
    kprintf("[writer %d] finished: see /tmp/writer%d.log\n", id, id);
}

static int cmd_spawn(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: spawn <program> [arg]\n"
                "  counter [n]   count once a second\n"
                "  hog           burn CPU forever (use ps / kill / nice)\n"
                "  prodcons      producer + consumer sharing a buffer (semaphores)\n"
                "  writer        two tasks writing files at the same time\n");
        return 1;
    }
    if (strcmp(argv[1], "counter") == 0) {
        pid_t p = task_create("counter", demo_counter, (void *)(uint32_t)(argc > 2 ? katoi(argv[2]) : 5));
        kprintf("started counter as pid %d\n", p);
    } else if (strcmp(argv[1], "hog") == 0) {
        kprintf("started hog as pid %d\n", task_create("hog", demo_hog, NULL));
    } else if (strcmp(argv[1], "prodcons") == 0) {
        sem_init(&items, 0);
        sem_init(&spaces, 4);
        rhead = rtail = 0;
        task_create("producer", producer, NULL);
        task_create("consumer", consumer, NULL);
        kprintf("started producer and consumer\n");
    } else if (strcmp(argv[1], "writer") == 0) {
        task_create("writer1", demo_writer, (void *)1);
        task_create("writer2", demo_writer, (void *)2);
        kprintf("started two writers\n");
    } else {
        kprintf("unknown program '%s'\n", argv[1]);
        return 1;
    }
    return 0;
}
SHELL_COMMAND("spawn", "spawn <program> [arg]", "Start a demo task", cmd_spawn);

static uint32_t prev_ticks[64];
static pid_t prev_pid[64];
static int prev_n, cur_n;
static uint32_t cur_ticks[64];
static pid_t cur_pid[64];

static void top_row(struct task *t, void *ctx)
{
    (void)ctx;
    uint32_t before = 0;

    for (int k = 0; k < prev_n; k++)
        if (prev_pid[k] == t->pid)
            before = prev_ticks[k];
    if (cur_n < 64) {
        cur_pid[cur_n] = t->pid;
        cur_ticks[cur_n] = t->cpu_ticks;
        cur_n++;
    }
    uint32_t delta = t->cpu_ticks - before;
    kprintf("  %3d  %-14s %-9s %3u%%\n", t->pid, t->name, state_name(t->state),
            delta > TIMER_HZ ? 100 : delta * 100 / TIMER_HZ);
}

static int cmd_top(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct input_event ev;

    prev_n = 0;
    for (;;) {
        console_clear();
        kprintf("top - up %u s, %u tasks, %s scheduler   (press q to quit)\n\n",
                timer_uptime_sec(), sched_task_count(), sched_get_policy()->name);
        kprintf("  PID  NAME           STATE      CPU\n");
        cur_n = 0;
        sched_for_each(top_row, NULL);
        memcpy(prev_pid, cur_pid, sizeof prev_pid);
        memcpy(prev_ticks, cur_ticks, sizeof prev_ticks);
        prev_n = cur_n;
        console_flush();
        if (input_wait(&ev, 1000) == 0 && ev.type == INPUT_KEY && (ev.key == 'q' || ev.key == 3))
            break;
    }
    console_clear();
    return 0;
}
SHELL_COMMAND("top", "top", "Live task monitor (q quits)", cmd_top);
