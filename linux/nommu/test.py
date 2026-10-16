"""tools/test_all.py on the no-MMU image (image.py): its workers bare (RVC_BARE_WORKERS), and cc's
programs flat ones (tcc -shared, elf2flt), whose bytes differ from the MMU machine's.
   python linux/nommu/test.py [--image build/images/nommu] [test_all.py's arguments]"""
import os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
s = open(os.path.join(ROOT, 'tools', 'test_all.py')).read()
s = s.replace("ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))", 'ROOT = ' + repr(ROOT))
old = "    env.pop('RVC_RACES', None)"
assert old in s
s = s.replace(old, old + "\n    env['RVC_BARE_WORKERS'] = '1'")
s, n = re.subn(r"(case\('cc-bytes'.*?digest=\[\(r'\^\(\[0-9a-f\]\{32\}  \\S\+\)', ')[0-9a-f]{8}('\)\])", r'\g<1>4f320f13\g<2>', s, count=1, flags=re.S)
assert n == 1, 'cc-bytes case not found'
if '--image' not in sys.argv:
    sys.argv[1:1] = ['--image', 'build/images/nommu']
exec(compile(s, os.path.join(ROOT, 'tools', 'test_all.py'), 'exec'), {'__name__': '__main__', '__file__': os.path.join(ROOT, 'tools', 'test_all.py')})
