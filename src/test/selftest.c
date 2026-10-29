/*
 * selftest.c - built-in tests. Run with the shell command `selftest`, or
 * add the word `selftest` to the kernel command line to run them at boot.
 *
 * Each CHECK prints PASS or FAIL. The summary line
 *     SELFTEST: PASS 42/42
 * is printed to the serial port too, so scripts can read it.
 */
#include "kernel.h"
#include "kprintf.h"
#include "kstring.h"
#include "bitmap.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"
#include "console.h"
#include "log.h"

static int checks, failures;

void selftest_check(bool ok, const char *name, const char *file, int line)
{
    checks++;
    if (ok) {
        kprintf("  [ ok ] %s\n", name);
    } else {
        failures++;
        console_set_color(CON_RED);
        kprintf("  [FAIL] %s  (%s:%d)\n", name, file, line);
        console_set_color(CON_NORMAL);
    }
}

#define CHECK(cond, name) selftest_check((cond), (name), __FILE__, __LINE__)

static void section(const char *s)
{
    console_set_color(CON_CYAN);
    kprintf("%s\n", s);
    console_set_color(CON_NORMAL);
}

/* ---------- lib ---------- */
static void test_lib(void)
{
    char b[64];

    section("lib");
    ksnprintf(b, sizeof b, "%d|%5d|%-5d|%05d|%+d", 42, 42, 42, 42, 42);
    CHECK(strcmp(b, "42|   42|42   |00042|+42") == 0, "printf integers");
    ksnprintf(b, sizeof b, "%x %X %o %u", 255, 255, 8, 4000000000u);
    CHECK(strcmp(b, "ff FF 10 4000000000") == 0, "printf bases");
    ksnprintf(b, sizeof b, "%llu %llx", 5000000000ull, 0x123456789ull);
    CHECK(strcmp(b, "5000000000 123456789") == 0, "printf 64-bit");
    ksnprintf(b, sizeof b, "[%s][%10s][%-6s][%.3s][%c]", "hi", "right", "left", "truncate", 'Z');
    CHECK(strcmp(b, "[hi][     right][left  ][tru][Z]") == 0, "printf strings");
    CHECK(strcmp("abc", "abd") < 0 && strlen("hello") == 5, "strcmp/strlen");
    CHECK(katoi("-123") == -123 && kstrtoul("0x1F", NULL, 0) == 31, "number parsing");

    uint32_t map[BITMAP_WORDS(100)];
    memset(map, 0, sizeof map);
    bitmap_set_range(map, 10, 5);
    CHECK(bitmap_test(map, 12) && !bitmap_test(map, 15), "bitmap set/test");
    CHECK(bitmap_find_clear(map, 100, 10) == 15, "bitmap find clear");
    CHECK(bitmap_find_clear_run(map, 100, 20) == 15, "bitmap find run");
    CHECK(bitmap_count_set(map, 100) == 5, "bitmap count");

    uint64_t big = 1000000000000ull;
    CHECK(big / 1000 == 1000000000ull && big % 7 == 1, "64-bit division");
}

/* ---------- memory ---------- */
static void test_mm(void)
{
    section("memory");
    uint32_t before = pmm_free_pages_count();
    paddr_t a = pmm_alloc_page(), b = pmm_alloc_page();
    CHECK(a && b && a != b && (a & 0xFFF) == 0, "pmm alloc distinct pages");
    CHECK(pmm_free_pages_count() == before - 2, "pmm free count drops");
    pmm_free_page(a);
    pmm_free_page(b);
    CHECK(pmm_free_pages_count() == before, "pmm free restores count");
    paddr_t c = pmm_alloc_contiguous(8);
    CHECK(c != 0, "pmm contiguous alloc");
    pmm_free_pages(c, 8);

    struct heap_stats s0, s1;
    heap_get_stats(&s0);
    void *p[64];
    for (int i = 0; i < 64; i++) {
        p[i] = kmalloc(16 + (size_t)i * 24);
        memset(p[i], i, 16 + (size_t)i * 24);
    }
    bool ok = true;
    for (int i = 0; i < 64; i++)
        ok &= ((uint8_t *)p[i])[0] == (uint8_t)i;
    CHECK(ok, "heap allocations keep their data");
    CHECK(heap_check() == 0, "heap structure valid");
    for (int i = 0; i < 64; i += 2)
        kfree(p[i]);
    for (int i = 1; i < 64; i += 2)
        kfree(p[i]);
    heap_get_stats(&s1);
    CHECK(s1.used_bytes == s0.used_bytes && s1.alloc_count == s0.alloc_count,
          "heap frees everything");
    CHECK(s1.largest_free >= s0.largest_free, "heap merges free blocks");

    char *r = kmalloc(8);
    strcpy(r, "grow");
    r = krealloc(r, 5000);
    CHECK(r && strcmp(r, "grow") == 0, "krealloc keeps data");
    kfree(r);

    CHECK(vmm_enabled() && !vmm_is_mapped(0), "paging on, page 0 unmapped");
    vaddr_t va = 0x20000000;                     /* 512 MiB: inside a large page */
    paddr_t pg = pmm_alloc_page();
    vmm_map(va, pg, VMM_WRITE);
    *(volatile uint32_t *)va = 0xCAFEBABE;
    CHECK(vmm_get_phys(va) == pg && *(volatile uint32_t *)pg == 0xCAFEBABE,
          "vmm_map remaps one page");
    vmm_unmap(va);
    CHECK(!vmm_is_mapped(va), "vmm_unmap");
    pmm_free_page(pg);
}

/* ---------- processes ---------- */
static volatile int counter_a, counter_b;
static struct mutex test_mutex;
static volatile int guarded;
static struct semaphore test_sem;
static volatile int sem_done;
static volatile int order_log[8], order_n;

static void count_a(void *arg)  { (void)arg; for (;;) counter_a++; }
static void count_b(void *arg)  { (void)arg; for (;;) counter_b++; }

static void mutex_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        mutex_lock(&test_mutex);
        int v = guarded;
        if (i % 100 == 0)
            task_yield();               /* try to provoke a race */
        guarded = v + 1;
        mutex_unlock(&test_mutex);
    }
}

static void sem_consumer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) {
        sem_down(&test_sem);
        sem_done++;
    }
}

static void exit_code_task(void *arg) { task_exit((int)(uint32_t)arg); }

static void sleeper(void *arg)
{
    int id = (int)(uint32_t)arg;
    timer_sleep_ms((uint32_t)id * 60);
    order_log[order_n++] = id;
}

static void test_proc(void)
{
    section("processes");

    pid_t a = task_create("count_a", count_a, NULL);
    pid_t b = task_create("count_b", count_b, NULL);
    timer_sleep_ms(300);
    CHECK(counter_a > 0 && counter_b > 0, "preemption: two busy tasks both run");
    task_kill(a);
    task_kill(b);
    int ca = counter_a;
    timer_sleep_ms(50);
    CHECK(ca == counter_a, "task_kill stops a task");

    mutex_init(&test_mutex);
    pid_t m1 = task_create_ex("mx1", mutex_worker, NULL, PRIO_DEFAULT, TASK_JOINABLE);
    pid_t m2 = task_create_ex("mx2", mutex_worker, NULL, PRIO_DEFAULT, TASK_JOINABLE);
    task_join(m1, NULL);
    task_join(m2, NULL);
    CHECK(guarded == 4000, "mutex protects a shared counter");

    sem_init(&test_sem, 0);
    pid_t sc = task_create_ex("sem", sem_consumer, NULL, PRIO_DEFAULT, TASK_JOINABLE);
    timer_sleep_ms(30);
    CHECK(sem_done == 0, "semaphore blocks while count is zero");
    sem_up(&test_sem); sem_up(&test_sem); sem_up(&test_sem);
    task_join(sc, NULL);
    CHECK(sem_done == 3, "semaphore releases waiters");

    int code = 0;
    pid_t e = task_create_ex("exit7", exit_code_task, (void *)7, PRIO_DEFAULT, TASK_JOINABLE);
    CHECK(task_join(e, &code) == 0 && code == 7, "task_join returns exit code");

    order_n = 0;
    pid_t s[3];
    for (int i = 3; i >= 1; i--)
        s[i - 1] = task_create_ex("sleeper", sleeper, (void *)(uint32_t)i,
                                  PRIO_DEFAULT, TASK_JOINABLE);
    for (int i = 0; i < 3; i++)
        task_join(s[i], NULL);
    CHECK(order_n == 3 && order_log[0] == 1 && order_log[1] == 2 &&
          order_log[2] == 3, "sleeping tasks wake in time order");

    uint32_t t0 = timer_ms();
    timer_sleep_ms(100);
    uint32_t dt = timer_ms() - t0;
    CHECK(dt >= 90 && dt <= 160, "timer_sleep_ms is about right");

    sched_set_policy(&sched_priority);
    CHECK(sched_get_policy() == &sched_priority, "switch to priority policy");
    counter_a = counter_b = 0;
    /* A strict priority policy starves low-priority tasks, so this test
     * task must itself have the top priority while it waits. */
    task_set_priority(task_current()->pid, 0);
    pid_t hi = task_create_ex("hi", count_a, NULL, 1, 0);
    pid_t lo = task_create_ex("lo", count_b, NULL, 3, 0);
    timer_sleep_ms(300);
    CHECK(counter_a > 0 && counter_b == 0, "priority policy: high priority wins");
    task_kill(hi);
    task_kill(lo);
    task_set_priority(task_current()->pid, PRIO_DEFAULT);
    sched_set_policy(&sched_round_robin);
    CHECK(sched_get_policy() == &sched_round_robin, "switch back to round-robin");
}

/* Hooks so later modules can add their tests. */
extern void selftest_fs(void) __attribute__((weak));
extern void selftest_gfx(void) __attribute__((weak));
extern void selftest_shell(void) __attribute__((weak));

int selftest_run(void)
{
    checks = failures = 0;
    console_set_color(CON_BRIGHT);
    kprintf("MyOS self test\n");
    console_set_color(CON_NORMAL);

    test_lib();
    test_mm();
    test_proc();
    if (selftest_fs)  { section("filesystem"); selftest_fs(); }
    if (selftest_gfx) { section("graphics");   selftest_gfx(); }
    if (selftest_shell) { section("shell");    selftest_shell(); }

    console_set_color(failures ? CON_RED : CON_GREEN);
    kprintf("SELFTEST: %s %d/%d\n", failures ? "FAIL" : "PASS",
            checks - failures, checks);
    console_set_color(CON_NORMAL);
    return failures;
}
