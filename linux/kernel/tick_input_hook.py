"""Has the kernel's tick poll the machine's input (shaderemu_input.c) and the idle loop arm
that driver's timer: a timer of its own that expired at every tick cost a running program
900 instructions a tick in the timer wheel, for a poll of 30. Idempotent."""
import sys
tree = sys.argv[1]
CALL = '#ifdef CONFIG_INPUT_SHADEREMU\n\t{\n\t\textern void %s(void);\n\t\t%s();\n\t}\n#endif\n'
for path, before, name in (
        ('/kernel/time/timer.c', '\taccount_process_tick(p, user_tick);\n', 'shaderemu_input_tick'),
        ('/kernel/sched/idle.c', '\ttick_nohz_idle_enter();\n', 'shaderemu_input_idle')):
    s = open(tree + path).read()
    if name in s:
        continue
    assert s.count(before) == 1, path
    open(tree + path, 'w').write(s.replace(before, CALL % (name, name) + before))
    print('input hook added to', path)
