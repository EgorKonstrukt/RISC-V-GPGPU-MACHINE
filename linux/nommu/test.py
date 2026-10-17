"""tools/test_all.py on the no-MMU image (image.py), which is its default once made; this names it.
   python linux/nommu/test.py [--image build/images/nommu] [test_all.py's arguments]"""
import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if '--image' not in sys.argv:
    sys.argv[1:1] = ['--image', 'build/images/nommu']
T = os.path.join(ROOT, 'tools', 'test_all.py')
exec(compile(open(T).read(), T, 'exec'), {'__name__': '__main__', '__file__': T})
