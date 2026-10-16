/* The start of a bFLT program for musl: the loader leaves argc, argv and envp on the stack but no
   auxiliary vector, which musl reads after envp; this copies them with one ending the list. */
int main();
__attribute__((weak)) void _init(void) {}
__attribute__((weak)) void _fini(void) {}
int __libc_start_main(int (*)(), int, char **, void (*)(void), void (*)(void), void (*)(void));

__attribute__((used)) void _start_c(long *p)
{
	int argc = p[0], envc = 0, i, n = 0;
	char **argv = (char **)(p + 1), **envp = argv + argc + 1;
	while (envp[envc]) envc++;
	long v[argc + envc + 6];
	for (i = 0; i < argc; i++) v[n++] = (long)argv[i];
	v[n++] = 0;
	for (i = 0; i < envc; i++) v[n++] = (long)envp[i];
	v[n++] = 0;
	v[n++] = 6; v[n++] = 4096;   /* AT_PAGESZ */
	v[n++] = 0; v[n++] = 0;
	__libc_start_main(main, argc, (char **)v, _init, _fini, 0);
}

__asm__(".text\n.global _start\n_start:\n.weak __global_pointer$\n.hidden __global_pointer$\n"
	".option push\n.option norelax\nlla gp, __global_pointer$\n.option pop\n"
	"mv a0, sp\nandi sp, sp, -16\ntail _start_c\n");

/* vfork as musl's riscv64 has it: clone(CLONE_VM | CLONE_VFORK | SIGCHLD); without an MMU there is no fork */
__asm__(".text\n.global vfork\n.type vfork,%function\nvfork:\nli a7, 220\nli a0, 0x4111\nmv a1, sp\necall\n"
	".hidden __syscall_ret\ntail __syscall_ret\n");
