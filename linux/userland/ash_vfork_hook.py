"""busybox's ash starts a plain command in the foreground with vfork (shell/ash.c). A fork
copied the shell's page tables and then its written pages, in the child and again in the
parent: a quarter of what an empty program cost from the prompt. Idempotent.

    python3 ash_vfork_hook.py BUSYBOX_SOURCE
The child writes nothing of the shell's memory but one word, the reason exec failed: the shell
then does it the old way, which says what was wrong (or runs a script that has no #! line).
"""
import re, sys

path = sys.argv[1] + '/shell/ash.c'
s = open(path).read()
if 'vforkexec' in s:
    print('ash vfork hook: there already')
    sys.exit()

function = r'''
/* ShaderEmu: a plain command in the foreground, by vfork. NULL: do it by fork. */
static volatile int vfork_errno;

/* The C library's vfork() is a fork on this machine. clone(CLONE_VM | CLONE_VFORK | SIGCHLD)
 * with no frame of its own: the child comes back through the same return address. */
__asm__(".text\n.type ash_vfork,%function\nash_vfork:\n"
	"\tli a7, 220\n\tli a0, 0x4111\n\tli a1, 0\n\tli a2, 0\n\tli a3, 0\n\tli a4, 0\n\tecall\n\tret\n");
pid_t ash_vfork(void) __attribute__((returns_twice));

static void
vfork_signal(int signo)
{
	/* what forkchild's setsignal() leaves a child with, of what this shell changed */
	if (sigmode[signo - 1] == S_IGN || sigmode[signo - 1] == S_CATCH)
		signal(signo, trap[signo] && trap[signo][0] == '\0' ? SIG_IGN : SIG_DFL);
}

static NOINLINE struct job *
vforkexec(union node *n, char **argv, const char *path, int idx)
{
	char **envp = listvars(VEXPORT, VUNSET, /*strlist:*/ NULL, /*end:*/ NULL);
	const char *cmdname = argv[0];
	struct job *jp;
	pid_t pid;

	if (strchr(argv[0], '/') == NULL) {
		cmdname = NULL;
		while (padvance(&path, argv[0]) >= 0) {
			if (--idx < 0 && pathopt == NULL) {
				cmdname = sstrdup(stackblock());
				break;
			}
		}
		if (cmdname == NULL)
			return NULL;
	}
	jp = makejob(/*n,*/ 1);
	vfork_errno = 0;
	pid = ash_vfork();
	if (pid == 0) {
		/* the kernel's side of forkchild(), and nothing of the shell's memory */
#if JOBS
		if (jp->jobctl && shlvl == 0) {
			setpgid(0, 0);
			tcsetpgrp(ttyfd, getpid());
			vfork_signal(SIGTSTP);
			vfork_signal(SIGTTOU);
		}
#endif
		if (shlvl == 0) {
			if (iflag) {
				vfork_signal(SIGINT);
				vfork_signal(SIGTERM);
			}
			vfork_signal(SIGQUIT);
		}
		execve(cmdname, argv, envp);
		vfork_errno = errno ? errno : ENOENT;
		_exit(127);
	}
	if (pid < 0 || vfork_errno) {
		if (pid > 0) {
			waitpid(pid, NULL, 0);
#if JOBS
			if (jp->jobctl && shlvl == 0)
				tcsetpgrp(ttyfd, rootpid);
#endif
		}
		freejob(jp);
		return NULL;
	}
	forkparent(jp, n, FORK_FG, pid);
	return jp;
}
'''
m = re.search(r'\nstatic (?:NOINLINE )?int\nevalcommand\(union node \*cmd, int flags\)\n', s)
if not m:
    sys.exit('ash vfork hook: evalcommand not found')
s = s[:m.start()] + '\n' + function + s[m.start():]
old = '''			INT_OFF;
			get_tty_state();
			jp = makejob(/*cmd,*/ 1);
			if (forkshell(jp, cmd, FORK_FG) != 0) {'''
if s.count(old) != 1:
    sys.exit('ash vfork hook: the fork of a plain command not found')
s = s.replace(old, '''			INT_OFF;
			get_tty_state();
			if ((jp = vforkexec(cmd, argv, path, cmdentry.u.index)) != NULL)
				break;
			jp = makejob(/*cmd,*/ 1);
			if (forkshell(jp, cmd, FORK_FG) != 0) {''')
open(path, 'w').write(s)
print('ash vfork hook added')
