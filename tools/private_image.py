"""An image of one's own for an experiment, beside the shared one: build/images/NAME.

    python tools/private_image.py NAME
    bin\\rvc_cpu.exe --image build/images/NAME ...

It is tools/make_linux_image.py's image of the shared tree (build/images/linux/root and
Image) as they are on disk, with two things of NAME's own when they are there: files under
build/images/NAME/overlay (at the paths they have in the image), which replace the shared
ones, and build/images/NAME/Image, a kernel. Nothing of build/images/linux is written, so
sessions that share it are not disturbed and a comparison keeps its "before".
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_linux_image as m

name = sys.argv[1]
shared = m.OUT
m.OUT = os.path.join(m.ROOT, 'build', 'images', name)
overlay = os.path.join(m.OUT, 'overlay')
m.ours = lambda n: os.path.join(m.OUT, n) if n == 'Image' and os.path.exists(os.path.join(m.OUT, n)) else os.path.join(shared, n)
shortcuts = m.desktop_shortcuts


def with_overlay(extra):
    """The desktop's shortcuts come last in the builder's list: the overlay after them wins."""
    files = shortcuts(extra)
    for folder, _, names in os.walk(overlay):
        directory = '/' + os.path.relpath(folder, overlay).replace(os.sep, '/')
        files += [(os.path.join(folder, n), directory, n) for n in sorted(names)]
    return files


m.desktop_shortcuts = with_overlay
m.main()
