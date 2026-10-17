"""The console writes without ending the machine's pass (docs/machine/boot.md, "Passes that end early").

The machine's buffer holds 64 characters a pass and the character that fills it ends the pass.
The driver (drivers/tty/hvc/hvc_riscv_sbi.c) counts what it has put in this pass (the timer's
value is one pass's: it changes when the next begins) and stops at 62; what is left waits in a
larger buffer of the tty's and the idle loop wakes the console's thread at the next pass. The
kernel's own messages cannot wait: for them the pass ends. Nothing of the machine changes. Idempotent."""
import sys
path = sys.argv[1] + '/drivers/tty/hvc/hvc_riscv_sbi.c'
s = open(path).read()
if 'ShaderEmu console' not in s:
    pairs = [
('''#include <linux/console.h>
''', '''#include <linux/console.h>
#include <linux/cpu.h>
'''),
('''#include <asm/sbi.h>
''', '''#include <asm/csr.h>
#include <asm/sbi.h>
'''),
('''static int hvc_sbi_tty_put(uint32_t vtermno, const char *buf, int count)
{
	int i;

	for (i = 0; i < count; i++)
		sbi_console_putchar(buf[i]);

	return i;
}
''', '''/*
 * ShaderEmu console: the machine takes 64 characters a pass and the 64th ends the pass. The
 * timer's value is the same for a whole pass, so a new value is a new pass with room again.
 */
#define HVC_SBI_ROOM 62

static unsigned long hvc_sbi_pass;	/* the timer's value when the count began */
static int hvc_sbi_put;			/* characters put in that pass */
static int hvc_sbi_waits;		/* the last write left characters behind */

static int hvc_sbi_tty_put(uint32_t vtermno, const char *buf, int count)
{
	unsigned long now = csr_read(CSR_TIME);
	int i;

	if (now != hvc_sbi_pass) {
		hvc_sbi_pass = now;
		hvc_sbi_put = 0;
	}
	for (i = 0; i < count && hvc_sbi_put < HVC_SBI_ROOM; i++, hvc_sbi_put++)
		sbi_console_putchar(buf[i]);
	hvc_sbi_waits = i < count;
	return i ? i : -EAGAIN;
}

/* The kernel's own messages are not put off (wait false): the pass ends, the next has room. */
static int hvc_sbi_flush(uint32_t vtermno, bool wait)
{
	if (!wait && hvc_sbi_waits)
		__asm__ __volatile__(".word 0x0100000f");	/* pause: the machine's pass ends */
	return 0;
}

/* A tty's output that is left is pushed by khvcd: woken here, once the idle loop's wait is over. */
void arch_cpu_idle_exit(void)
{
	if (hvc_sbi_waits)
		hvc_kick();
}
'''),
('''	.put_chars = hvc_sbi_tty_put,
''', '''	.put_chars = hvc_sbi_tty_put,
	.flush = hvc_sbi_flush,
'''),
('''hvc_alloc(0, 0, &hvc_sbi_ops, 16)''', '''hvc_alloc(0, 0, &hvc_sbi_ops, 1024)'''),
    ]
    for old, new in pairs:
        assert s.count(old) == 1, old
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print('console hook: drivers/tty/hvc/hvc_riscv_sbi.c')
