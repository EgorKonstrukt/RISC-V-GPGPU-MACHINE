"""Prints the browser's home page: the pages in the image, and the sites of sites.txt
(one "address<TAB>name" a line)."""
import html
import sys

sites = [line.rstrip('\n').split('\t') for line in open(sys.argv[1], encoding='utf-8') if '\t' in line]
print('<html><head><title>Home</title><style>body { font-family: sans-serif } h1 { font-size: 24px }')
print('h2 { font-size: 17px; border-bottom: 1px solid #c0c0c0 } li { margin: 2px 0 }</style></head><body>')
print('<h1>ShaderEmu Web</h1>')
print('<p>A browser for pages as they are written: text in their own faces and sizes and in any')
print('script, pictures, tables, boxes side by side, forms that search. No scripts are run: a page')
print('that is nothing without them shows what it says to readers who have none.</p>')
print('<p><b>In VRChat the addresses listed on this page open at a click</b>, with their style sheets')
print('and most of their pictures; another address needs one step by hand. VRChat does not let a')
print('world open an address by itself, only those it was made with and one a visitor gives it. So')
print('when you follow a link to somewhere else, or type an address above, a button appears by the')
print('display: it opens a panel with the address to copy and a field to paste it into, and the')
print('page opens. That is a rule of VRChat, not a fault of this computer.</p>')
print('<h2>In this computer</h2><ul>')
print('<li><a href="web-about.html">About this machine</a></li>')
print('<li><a href="web-layout.html">Layout and fonts: what the browser lays out, and every script it writes</a></li>')
print('<li><a href="web-test.html">A test page: every kind of thing the browser shows</a></li>')
print('<li><a href="web-long.html">A long page, for scrolling</a></li>')
print('<li><a href="web-css.html">Style sheets: what the browser does with them</a></li>')
print('</ul>')
print('<h2>On the network</h2>')
print('<p>The machine has no network of its own: it asks its host, which fetches the page and')
print('puts it in memory.</p><ul>')
for address, name in sites:
    print('<li><a href="%s">%s</a></li>' % (html.escape(address), html.escape(name)))
print('<li><a href="world:typed">The address typed on the panel in the room</a> (VRChat)</li>')
print('</ul>')
print('<p>Outside VRChat (in the harness) any address works: click the address above and type one.</p>')
print('</body></html>')
