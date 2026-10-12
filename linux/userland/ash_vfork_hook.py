"""busybox's ash starts a plain command with vfork (shell/ash.c), in the foreground or, when
its words are names only, with `&`. A fork
copied the shell's page tables and then its written pages, in the child and again in the
parent: a quarter of what an empty program cost from the prompt. Idempotent.

    python3 ash_vfork_hook.py BUSYBOX_SOURCE
The child writes nothing of the shell's memory but one word, the reason exec failed: the shell
then does it the old way, which says what was wrong (or runs a script that has no #! line).
"""
import re, sys

path = sys.argv[1] + '/shell/ash.c'
s = open(path).read()
if 'vfork_background' in s:
    print('ash vfork hook: there already')
    sys.exit()
EVALCOMMAND = r'\nstatic (?:NOINLINE )?int\nevalcommand\(union node \*cmd, int flags\)\n'
OLD = '\n/* ShaderEmu: a plain command in the foreground, by vfork. NULL: do it by fork. */'

function = r'''
/* ShaderEmu: a plain command by vfork, in the foreground or (FORK_BG) not. NULL: do it by fork. */
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
vfork_start(union node *n, char **argv, const char *path, int idx, int mode)
{
	char **envp = listvars(VEXPORT, VUNSET, /*strlist:*/ NULL, /*end:*/ NULL);
	const char *cmdname = argv[0];
	struct job *jp;
	int control = 0, quiet;
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
#if JOBS
	control = jp->jobctl && shlvl == 0;
#endif
	/* (a background command of a shell without job control: no SIGINT, SIGQUIT or console) */
	quiet = mode == FORK_BG && !control;
	vfork_errno = 0;
	pid = ash_vfork();
	if (pid == 0) {
		/* the kernel's side of forkchild(), and nothing of the shell's memory */
		if (control) {
			setpgid(0, 0);
			if (mode == FORK_FG)
				tcsetpgrp(ttyfd, getpid());
			vfork_signal(SIGTSTP);
			vfork_signal(SIGTTOU);
		}
		if (quiet) {
			signal(SIGINT, SIG_IGN);
			signal(SIGQUIT, SIG_IGN);
			close(0);
			if (open(bb_dev_null, O_RDONLY) != 0) {
				vfork_errno = EIO;
				_exit(127);
			}
		}
		if (shlvl == 0) {
			if (iflag) {
				if (!quiet)
					vfork_signal(SIGINT);
				vfork_signal(SIGTERM);
			}
			if (!quiet)
				vfork_signal(SIGQUIT);
		}
		execve(cmdname, argv, envp);
		vfork_errno = errno ? errno : ENOENT;
		_exit(127);
	}
	if (pid < 0 || vfork_errno) {
		if (pid > 0) {
			waitpid(pid, NULL, 0);
			if (control && mode == FORK_FG)
				tcsetpgrp(ttyfd, rootpid);
		}
		freejob(jp);
		return NULL;
	}
	forkparent(jp, n, mode, pid);
	return jp;
}

static struct job *
vforkexec(union node *n, char **argv, const char *path, int idx)
{
	return vfork_start(n, argv, path, idx, FORK_FG);
}

/* `command &` whose words have nothing to expand but names: by vfork too. 0: do it by fork. */
static int
vfork_background(union node *n)
{
	union node *cmd = n->nredir.n, *argp;
	struct cmdentry entry;
	struct arglist arglist;
	struct strlist *sp;
	struct stackmark smark;
	struct job *jp = NULL;
	char **argv;
	const char *p;
	int argc = 0;

	if (n->nredir.redirect || !cmd || cmd->type != NCMD || cmd->ncmd.assign || cmd->ncmd.redirect || !cmd->ncmd.args)
		return 0;
	for (argp = cmd->ncmd.args; argp; argp = argp->narg.next) {
		if (argp->narg.backquote)
			return 0;
		for (p = argp->narg.text; *p; p++)
			if ((unsigned char)*p >= CTL_FIRST && (unsigned char)*p <= CTL_LAST)
				return 0;
	}
	setstackmark(&smark);
	arglist.lastp = &arglist.list;
	for (argp = cmd->ncmd.args; argp; argp = argp->narg.next)
		expandarg(argp, &arglist, EXP_FULL | EXP_TILDE);
	*arglist.lastp = NULL;
	for (sp = arglist.list; sp; sp = sp->next)
		argc++;
	argv = stalloc(sizeof(char *) * (argc + 1));
	for (argc = 0, sp = arglist.list; sp; sp = sp->next)
		argv[argc++] = sp->text;
	argv[argc] = NULL;
	if (argc) {
		find_command(argv[0], &entry, 0, pathval());
		if (entry.cmdtype == CMDNORMAL)
			jp = vfork_start(n, argv, pathval(), entry.u.index, FORK_BG);
	}
	popstackmark(&smark);
	return jp != NULL;
}
'''
m = re.search(EVALCOMMAND, s)
if not m:
    sys.exit('ash vfork hook: evalcommand not found')
had = OLD in s
if had:
    # (a tree with the foreground's hook alone: its function goes, its call stays)
    s = s[:s.index(OLD)] + s[m.start():]
    m = re.search(EVALCOMMAND, s)
s = s[:m.start()] + '\n' + function + s[m.start():]
old = '''	INT_OFF;
	if (backgnd == FORK_FG)
		get_tty_state();
	jp = makejob(/*n,*/ 1);
	if (forkshell(jp, n, backgnd) == 0) {'''
if s.count(old) != 1:
    sys.exit('ash vfork hook: the fork of a subshell not found')
s = s.replace(old, '''	INT_OFF;
	if (backgnd && vfork_background(n)) {
		INT_ON;
		return 0;
	}
	if (backgnd == FORK_FG)
		get_tty_state();
	jp = makejob(/*n,*/ 1);
	if (forkshell(jp, n, backgnd) == 0) {''')
old = '\nstatic int\nevalsubshell(union node *n, int flags)\n'
if s.count(old) != 1:
    sys.exit('ash vfork hook: evalsubshell not found')
s = s.replace(old, '\nstatic int vfork_background(union node *n);\n' + old)
old = '''			INT_OFF;
			get_tty_state();
			jp = makejob(/*cmd,*/ 1);
			if (forkshell(jp, cmd, FORK_FG) != 0) {'''
if not had and s.count(old) != 1:
    sys.exit('ash vfork hook: the fork of a plain command not found')
s = s if had else s.replace(old, '''			INT_OFF;
			get_tty_state();
			if ((jp = vforkexec(cmd, argv, path, cmdentry.u.index)) != NULL)
				break;
			jp = makejob(/*cmd,*/ 1);
			if (forkshell(jp, cmd, FORK_FG) != 0) {''')
open(path, 'w').write(s)
print('ash vfork hook added')
