/*
 * switch.S - switch from one task's kernel stack to another's.
 *
 *   void switch_context(uint32_t *save_old_esp, uint32_t new_esp);
 *
 * Saves the callee-saved registers on the old stack, stores the old stack
 * pointer, loads the new one, and restores that task's registers. The
 * final `ret` continues the new task right where it last called this.
 */
    .global switch_context
switch_context:
    movl 4(%esp), %eax          /* where to store old esp */
    movl 8(%esp), %edx          /* new esp                */
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, (%eax)
    movl %edx, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
