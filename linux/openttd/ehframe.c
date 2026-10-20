/* The unwinder's tables for a flat program, which keeps no program headers to find them by: as
 * gcc's crtbegin and crtend, a mark at the start of .eh_frame (linked first, -DEH_BEGIN) registered
 * before any constructor, and a zero word at its end (linked last). No unwind tables of its own. */
#ifdef EH_BEGIN
static const char eh_begin[] __attribute__((section(".eh_frame"), aligned(4), used)) = {};
void __register_frame_info(const void *begin, void *object);

__attribute__((constructor(101))) static void eh_register(void)
{
	static long object[8];   /* libgcc's struct object: six words */

	__register_frame_info(eh_begin, object);
}
#else
static const int eh_end[1] __attribute__((section(".eh_frame"), aligned(4), used)) = {0};
#endif
