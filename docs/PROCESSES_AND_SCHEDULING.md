# Processes and scheduling

A task has its own kernel stack. `task_create(name, fn, arg)` starts one; the timer interrupt
preempts every 10 ms and `switch_context` (switch.S) swaps stacks.

A scheduler policy is 4 functions: enqueue (task became ready), dequeue, pick (choose next,
remove it from the queue), tick (every timer tick; return true to preempt).
Copy `src/user/my_scheduler.c`, change `my_pick`, rebuild, then run `sched mine`.
Built-in: `sched rr`, `sched prio`. Demo tasks: `spawn counter|hog|prodcons|writer`, `ps`, `top`, `nice`, `kill`.

Synchronisation: `mutex_*`, `sem_*`, `wq_*` in include/sync.h.
Rule: the idle task (pid 0) must never block.
