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
    """The desktop's shortcuts come last in the builder's list: the overlay after them wins.
    They are made of the Start menu's lists as the image will have them: the overlay's too."""
    import shutil, tempfile
    with tempfile.TemporaryDirectory() as both:
        os.makedirs(os.path.join(both, 'usr', 'share'))
        for _, _, run in m.PLACES:
            if os.path.isdir(os.path.join(extra, *run.split(' ', 1)[1].split('/'))):
                os.makedirs(os.path.join(both, *run.split(' ', 1)[1].split('/')), exist_ok=True)
        for share in (os.path.join(extra, 'usr', 'share'), os.path.join(overlay, 'usr', 'share')):
            for n in os.listdir(share) if os.path.isdir(share) else []:
                if n.startswith('nxapps.'):
                    shutil.copy(os.path.join(share, n), os.path.join(both, 'usr', 'share', n))
        files = shortcuts(both)
    for folder, _, names in os.walk(overlay):
        directory = '/' + os.path.relpath(folder, overlay).replace(os.sep, '/')
        files += [(os.path.join(folder, n), directory, n) for n in sorted(names)]
    return files


m.desktop_shortcuts = with_overlay
# icon sheets of the overlay's own are the ones the programs' icons are cut from
icons = m.with_icon
own = os.path.join(overlay, 'usr', 'share')
m.with_icon = lambda share, name, content: icons(own if os.path.exists(os.path.join(own, 'icons.idx')) else share, name, content)
m.main()
