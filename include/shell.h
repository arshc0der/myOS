/*
 * shell.h - the command line.
 *
 * ================= HOW TO ADD YOUR OWN COMMAND =================
 *
 *   static int cmd_hello(int argc, char **argv)
 *   {
 *       kprintf("Hello, %s!\n", argc > 1 ? argv[1] : "world");
 *       return 0;                    // 0 = success, anything else = failure
 *   }
 *   SHELL_COMMAND("hello", "hello [name]", "Say hello", cmd_hello);
 *
 * Put it in any .c file under src/ (src/user/ is the place for yours). It
 * appears in `help` automatically; nothing else to register.
 * ================================================================
 */
#ifndef SHELL_H
#define SHELL_H
#include "types.h"

struct shell_cmd {
    const char *name;
    const char *usage;
    const char *help;
    int (*fn)(int argc, char **argv);
};

#define SHELL_COMMAND(name_, usage_, help_, fn_) \
    static const struct shell_cmd __shell_cmd_##fn_ \
        __attribute__((section(".shell_cmds"), used)) = \
        { name_, usage_, help_, fn_ }

void shell_run(void);                           /* the interactive loop (never returns) */
int  shell_exec(const char *line);              /* run one command line; returns its status */
const struct shell_cmd *shell_find(const char *name);
void shell_for_each(void (*cb)(const struct shell_cmd *, void *), void *ctx);

#endif
