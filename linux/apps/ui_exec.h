/* ui_exec.h: why a launcher's child could not become the program. The child of a vfork shares
 * the launcher's memory until its exec, so it writes the exec's error here before it gives up,
 * and the launcher reads it once vfork returns (ui_launch.h reports it). */
#ifndef UI_EXEC_H
#define UI_EXEC_H
#include <errno.h>
#include <unistd.h>

static volatile int ui_exec_errno;

static void __attribute__((noreturn, unused))
ui_exec_failed(void)
{
	ui_exec_errno = errno ? errno : ENOEXEC;
	_exit(127);
}
#endif
