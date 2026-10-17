/* No MMU: a fork is a vfork, whose child runs on its parent's memory until it execs or exits. */
#ifndef __ASSEMBLER__
#include <sys/types.h>
pid_t vfork(void) __attribute__((returns_twice));
#define fork vfork
#endif
