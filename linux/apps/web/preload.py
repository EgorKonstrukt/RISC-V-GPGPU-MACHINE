#!/usr/bin/env python3
"""Preloads the browser's sites: what nxweb would ask its host for, found ahead of time.

Writes build/web/links.txt (kind<TAB>address: P page, S sheet, I picture), its stable part,
and with --cache DIR a cache for the ROM (web-cache.idx, webc-NNNN.*; see build_cache).
"""

import argparse
import collections
import datetime
import io
import os
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

try:
    from PIL import Image
except ImportError:
    Image = None
try:
    import cairosvg
except Exception:
    cairosvg = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/126.0 Safari/537.36")
PAGE_MOST = 3 << 19         # nxweb.c: bytes of a page that are read
SHEET_MOST = 3 << 18        # nxweb.c: bytes of a sheet that are read
WEB_SHEETS = 16             # web.h: a page's sheets, its own <style> text among them
MAX_PICTURES = 160          # nxweb.c: a page's distinct pictures
WEB_DEPTH = 200             # web.h: elements open inside one another
CSS_EM = 13                 # css.h: our pixels to a page's 16
PROBE = 32768               # bytes of a picture read to learn its kind and size
PICTURE_READ = 8000000
PPM_MOST = 4000000
WIDEST = 800
WORKERS = 16
PER_HOST = 2
SPACE = b" \t\n\v\f\r"

deadline = 0.0


# ---- web.h and css.h, ported ----

def load_entities():
    """web_entities[] of web.h, read where it lies: the two cannot come apart."""
    table = {}
    try:
        with open(os.path.join(HERE, "..", "web.h"), "rb") as f:
            text = f.read()
        text = text[text.index(b"web_entities[] = {"):]
        for name, code in re.findall(rb"\{ \"(\w+)\", (0x[0-9a-f]+|'\\?.') \}", text[:text.index(b"};")]):
            table[name] = int(code, 16) if code.startswith(b"0x") else code[-2]
    except (OSError, ValueError):
        pass
    if len(table) < 100:
        import html.entities
        print("web.h's entities not found: HTML 4's names are used", file=sys.stderr)
        table = {k.encode(): v for k, v in html.entities.name2codepoint.items() if v <= 0xffff}
        table[b"apos"] = 0x27
    return table


ENTITIES = load_entities()
DIGITS, HEX = b"0123456789", b"0123456789abcdefABCDEF"


def web_entity(at):
    """web_entity(): the character an entity at at[0] stands for and its length, or (0, 1)."""
    left, n = len(at), 1
    if left > 2 and at[1] == 0x23:
        base = 16 if at[2] in b"xX" else 10
        first = n = 2 + (base == 16)
        code = 0
        while n < left and n < 12 and at[n] in (HEX if base == 16 else DIGITS):
            code = (code * base + int(at[n:n + 1], 16)) & 0xffffffff
            n += 1
        if n == first:
            return 0, 1
        code -= (code & 0x80000000) << 1        # (an int of the C's)
        return (0xfffd if code > 0xffff or code < 9 else code), n + (at[n:n + 1] == b";")
    while n < left and n < 10 and at[n:n + 1].isalnum():
        n += 1
    if n >= left or at[n] != 0x3b:
        return 0, 1
    code = ENTITIES.get(at[1:n], 0)
    return (code, n + 1) if code else (0, 1)


def web_attr_text(value, room):
    """web_attr_text(): a value trimmed, its entities as UTF-8, its tabs and line ends gone."""
    value = value.strip(SPACE)
    out, i, n = bytearray(), 0, len(value)
    while i < n and len(out) < room - 4:
        code, used = value[i], 1
        if code == 0x26:
            code, used = web_entity(value[i:i + 14])
            if not code:
                code, used = 0x26, 1
        if code in (9, 10, 13):
            i += used
            continue
        out += bytes((code,)) if code < 0x80 or value[i] >= 0x80 else chr(code).encode("utf-8", "surrogatepass")
        i += used
    return bytes(out)


def css_attribute(tag, name):
    """css_attribute(): the first attribute of that name in a tag's text, or None."""
    n, m, i = len(tag), len(name), 0
    while i < n:
        while i < n and (tag[i] in SPACE or tag[i] == 0x2f):
            i += 1
        start = i
        while i < n and tag[i] not in SPACE and tag[i] != 0x3d and tag[i] != 0x2f:
            i += 1
        stop = i
        while i < n and tag[i] in SPACE:
            i += 1
        value = b""
        if i < n and tag[i] == 0x3d:
            i += 1
            while i < n and tag[i] in SPACE:
                i += 1
            if i < n and tag[i] in b"\"'":
                close = tag.find(tag[i:i + 1], i + 1)
                close = n if close < 0 else close
                value, i = tag[i + 1:close], close + 1
            else:
                first = i
                while i < n and tag[i] not in SPACE:
                    i += 1
                value = tag[first:i]
        elif stop == start:
            i += 1
        if stop - start == m and tag[start:stop].lower() == name:
            return value
    return None


def web_resolve(href, address, base):
    """web_resolve(): an address a page gives, made whole: (it, ""), or (None, why not).
    `base` is the page's <base href> made whole, or b"". The why is "" for what is no address."""
    href = href.lstrip(SPACE)
    low = href[:11].lower()
    if not href or low.startswith((b"data:", b"javascript:")):
        return None, ""
    n = 0
    while n < len(href) and (href[n:n + 1].isalnum() or href[n] in b"+-."):
        n += 1
    base = (base or address)[:255].split(b"#", 1)[0]
    if n > 1 and href[n:n + 1] == b":":
        if not low.startswith((b"http:", b"https:", b"world:", b"file:")):
            return None, ""
        joined = href[7:] if low.startswith(b"file://") else href
    else:
        host = base.find(b"://")
        query = base.find(b"?", host + 3 if host >= 0 else 0)
        if query >= 0:
            base = base[:query]
        if href.startswith(b"//") and host >= 0:
            joined = base[:host + 1] + href
        elif href[:1] == b"/":
            # from the site's root
            slash = base.find(b"/", host + 3) if host >= 0 else -1
            joined = (base[:slash] if slash >= 0 else base if host >= 0 else b"") + href
        elif href[:1] in (b"?", b"#"):
            joined = base + href
        else:
            slash = base.rfind(b"/")
            joined = (base[:slash + 1] if slash >= 0 and (host < 0 or slash > host + 2) else base + b"/") + href
    joined = joined[:599]
    # the path's ./ and ../ are taken out
    host = joined.find(b"://")
    slash = joined.find(b"/", host + 3) if host >= 0 else -1
    at = slash if slash >= 0 else 0 if joined[:1] == b"/" else len(joined)
    out, n = bytearray(), len(joined)
    while at < n and joined[at] not in b"?#":
        if joined.startswith(b"/.", at) and joined[at + 2:at + 3] in (b"/", b"", b"?", b"#"):
            at += 2
            if joined[at:at + 1] != b"/":
                out += b"/"
        elif joined.startswith(b"/..", at) and joined[at + 3:at + 4] in (b"/", b"", b"?", b"#"):
            at += 3
            while out and out.pop() != 0x2f:
                pass
            if joined[at:at + 1] != b"/":
                out += b"/"
        else:
            out.append(joined[at])
            at += 1
    if slash >= 0 or joined[:1] == b"/":
        joined = joined[:max(slash, 0)] + bytes(out) + joined[at:]
    if host >= 0 and b"/" not in joined[host + 3:] and len(joined) < 598:
        # a site with no path is its root
        cut = min((k for k in (joined.find(b"?"), joined.find(b"#")) if k >= 0), default=len(joined))
        joined = joined[:cut] + b"/" + joined[cut:]
    if any(c <= 0x20 for c in joined):
        return None, "spaces or control characters"
    if len(joined) > 255:
        return None, "over 255 bytes"
    return joined, ""


NUMBER = re.compile(rb"([+-]?)(\d*)(?:\.(\d*))?")


def css_number(s):
    """css_number(): a number in thousandths and what follows it, or None."""
    sign, whole, part = NUMBER.match(s).groups()
    if not whole and not part:
        return None
    v = 0
    for digit in whole:
        if v < 100000:
            v = v * 10 + digit - 48
    v *= 1000
    for k, digit in enumerate((part or b"")[:3]):
        v += (digit - 48) * (100, 10, 1)[k]
    return (-v if sign == b"-" else v), s[NUMBER.match(s).end():]


def css_media(s, view):
    """css_media() of css.h: whether a media query's list holds for a page area `view` wide."""
    n, i = len(s), 0
    ok, found, negate, seen = 1, 0, 0, 0
    wide = view * 16 // CSS_EM      # the page's pixels: ours are 13 to its 16

    def holds(text, most):
        number = css_number(text)
        if not number:
            return 0
        m = number[0] * 16 if number[1][:2].lower() == b"em" or number[1][:3].lower() == b"rem" else number[0]
        m = int(m / 1000)
        return wide <= m if most else wide >= m

    while True:
        if i >= n or s[i] == 0x2c:
            if seen and (not ok if negate else ok):
                found = 1
            if i >= n:
                break
            ok, negate, seen = 1, 0, 0
            i += 1
            continue
        if s[i] in SPACE:
            i += 1
            continue
        seen = 1
        if s[i] == 0x28:
            end = s.find(b")", i)
            end = n if end < 0 else end
            p = s[i + 1:end].lstrip(SPACE)
            low = p.lower()
            most = low.startswith(b"max-")
            if (most or low.startswith(b"min-")) and low.startswith((b"width", b"device-width"), 4):
                colon = p.find(b":")
                ok &= holds(p[colon + 1:].lstrip(SPACE), most) if colon >= 0 else 0
            elif low.startswith(b"width"):
                # width <= 600px, width >= 40em
                q = p[5:].lstrip(SPACE)
                if q[:1] in (b"<", b">"):
                    ok &= holds(q[2 if q[1:2] == b"=" else 1:].lstrip(SPACE), q[:1] == b"<")
                else:
                    ok = 0
            elif low.startswith(b"prefers-color-scheme"):
                ok &= b"light" in p
            elif low.startswith(b"orientation"):
                ok &= b"landscape" in p
            elif low.startswith(b"prefers-reduced-motion"):
                ok &= b"reduce" in p
            elif low.startswith(b"scripting"):
                ok &= b"none" in p
            elif low.startswith((b"hover", b"any-hover", b"pointer")):
                ok &= b"none" not in p and b"coarse" not in p
            else:
                ok = 0
            i = end + 1
            continue
        if s[i:i + 3].lower() == b"not" and not s[i + 3:i + 4].isalnum():
            negate = 1
            i += 2
        elif s[i:i + 5].lower() == b"print" or s[i:i + 6].lower() == b"speech":
            ok = 0
        while i + 1 < n and s[i + 1:i + 2].isalnum():
            i += 1
        i += 1
    return bool(found or n == 0)


def web_find_sheets(page, address, most, view):
    """web_find_sheets(): the linked sheets the browser asks for, in order, and what was lost.
    It reads the raw HTML: comments are passed over, a <noscript>'s sheets are taken."""
    low, n, i, count, base = page.lower(), len(page), 0, 0, b""
    out, lost = [], collections.Counter()
    while i + 7 < n and count < most:
        i = page.find(b"<", i)
        if i < 0 or i + 7 >= n:
            break
        if page.startswith(b"<!--", i):
            close = page.find(b"-->", i)
            if close < 0:
                break
            i = close + 3
            continue
        if not low.startswith((b"<style", b"<link", b"<base"), i):
            i += 1
            continue
        end = page.find(b">", i)
        end = n if end < 0 else end
        if low[i + 1] == 0x62:
            # the first <base href> that is an address: not decoded here, as in the C
            value = css_attribute(page[i + 5:end], b"href") if not base else None
            if value is not None and len(value) < 250:
                base = web_resolve(value, address, b"")[0] or b""
            i = end + 1
            continue
        if low[i + 1] == 0x73:
            close = low.find(b"</style", end, n - 1)
            if close < 0:
                break
            media = css_attribute(page[i + 6:end], b"media")
            if media is None or css_media(media, view):
                count += 1
            i = close + 1
            continue
        tag = page[i + 5:end]
        rel = css_attribute(tag, b"rel")
        if rel is not None and b"stylesheet" in rel and b"alternate" not in rel:
            media = css_attribute(tag, b"media")
            href = css_attribute(tag, b"href") if media is None or css_media(media, view) else None
            if href is not None and len(href) < 590:
                # (only &amp; is decoded in a sheet's address: web_find_sheets has no node)
                whole, why = web_resolve(href.replace(b"&amp;", b"&"), address, base)
                if whole is not None:
                    count += 1
                    out.append(whole)
                elif why:
                    lost[why] += 1
        i = end + 1
    return out, lost


def words(text):
    return frozenset(text.encode().split())


TAG_OPEN = re.compile(rb"<[A-Za-z/!?]")
TAG_STOP = re.compile(rb">|(?<==)[\"']")
NAME = re.compile(rb"[0-9A-Za-z:-]*")
# web_plain_style(): the tags that are display none before any sheet
UNSEEN = words("head script style title meta link template svg iframe video audio canvas object embed param "
               "source track area map col colgroup base datalist option math")
VOID = words("br img hr input meta link source wbr area col embed param track base")
ENDS_P = words("p div ul ol table pre blockquote hr h1 h2 h3 h4 h5 h6 section article header footer form dl nav "
               "main aside figure address fieldset details center menu hgroup figcaption")
OF_HEAD = words("head title meta link style script base noscript")
# web_parse(): what a new element ends without saying so (which elements, and inside what)
CLOSES = {
    b"li": ((words("li"), words("ul ol")),), b"dt": ((words("dt dd"), words("dl")),),
    b"dd": ((words("dt dd"), words("dl")),), b"tr": ((words("tr"), words("table")),),
    b"td": ((words("td th"), words("tr table")),), b"th": ((words("td th"), words("tr table")),),
    b"option": ((words("option"), words("select")),), b"optgroup": ((words("option"), words("select")),),
    b"a": ((words("a"), words("table td")),),
}
for group in (b"thead", b"tbody", b"tfoot"):
    CLOSES[group] = ((words("tr"), words("table")), (words("thead tbody"), words("table")), (words("tfoot"), words("table")))
PARAGRAPH = (words("p"), words("table td"))


def tag_end(page, at, n):
    """Where a tag's > is: one inside a value quoted after its = is not it."""
    while True:
        m = TAG_STOP.search(page, at)
        if not m:
            return n
        if page[m.start()] == 0x3e:
            return m.start()
        close = page.find(m.group(), m.end())
        if close < 0:
            return n
        at = close + 1


def small(attrs):
    """Whether a picture's width and height attributes are both 2 or under: it is not drawn."""
    for name in (b"width", b"height"):
        value = css_attribute(attrs, name)
        digits = re.match(rb"\d+", value or b"")
        if not digits or int(digits.group()) > 2 or value[digits.end():digits.end() + 1] == b"%":
            return False
    return True


def web_parse(page, address):
    """web_parse() and what web_style_all() hides without a sheet: the <base>, the pictures
    (src, small) and links' href of elements that are laid out, in the page's order.
    What a sheet hides is not known here: this can find more than the browser shows."""
    low, n, i = page.lower(), len(page), 0
    stack = [(b"html", False, False)]      # name, nothing inside is shown, a closed <details>
    body, base, pictures, links = False, b"", [], []
    while True:
        m = TAG_OPEN.search(page, i)
        if not m:
            break
        i = m.start()
        if page.startswith(b"<!--", i):
            close = page.find(b"-->", i + 4)
            i = n if close < 0 else close + 3
            continue
        end = tag_end(page, i + 1, n)
        closing = page[i + 1] == 0x2f
        name_end = NAME.match(page, i + 1 + closing, end).end()
        name = low[i + 1 + closing:name_end]
        if page[i + 1] in b"!?" or not name or len(name) > 32:
            i = end + 1
            continue
        i = end + 1
        if closing:
            for k in range(len(stack) - 1, 0, -1):
                if stack[k][0] == name:
                    del stack[k:]
                    break
            continue
        if name in (b"html", b"body"):
            if name == b"body" and not body:
                del stack[1:]
                stack.append((b"body", False, False))
                body = True
            continue
        for inner, stops in CLOSES.get(name, ()) + ((PARAGRAPH,) if name in ENDS_P else ()):
            for k in range(len(stack) - 1, 0, -1):
                if stack[k][0] in stops:
                    break
                if stack[k][0] in inner:
                    del stack[k:]
                    break
        if not body and name not in OF_HEAD:
            # the page's first content: a head that was never closed ends here
            del stack[1:]
            stack.append((b"body", False, False))
            body = True
        attrs = page[name_end:end]
        closed = bool(attrs) and attrs[-1] == 0x2f
        if closed:
            attrs = attrs[:-1]
        top = stack[-1]
        unseen = (top[1] or (top[2] and name != b"summary") or name in UNSEEN or
                  (name == b"noscript" and top[0] == b"head") or
                  (name == b"dialog" and css_attribute(attrs, b"open") is None) or
                  (b"hidden" in attrs.lower() and css_attribute(attrs, b"hidden") is not None))
        if name == b"base" and not base:
            value = css_attribute(attrs, b"href")
            if value is not None:
                base = web_resolve(web_attr_text(value, 256), address, b"")[0] or b""
        if name in (b"script", b"style"):
            # its text is not tags
            close = low.find(b"</script" if name == b"script" else b"</style", i)
            i = n if close < 0 else close
            continue
        if not unseen and name in (b"img", b"a"):
            value = css_attribute(attrs, b"src" if name == b"img" else b"href")
            if value is not None and name == b"img":
                pictures.append((value, small(attrs)))
            elif value is not None:
                links.append(value)
        if name not in VOID and not closed and len(stack) < WEB_DEPTH - 1:
            # (a field's own content is not laid out)
            stack.append((name, unseen or name in (b"select", b"textarea"),
                          name == b"details" and css_attribute(attrs, b"open") is None))
    return base, pictures, links


def page_asks(page, address, most_pictures):
    """What the layout asks web_picture() for, and the addresses the page's links go to:
    [(whole, small)], [whole without its fragment], what was lost and why."""
    base, found, hrefs = web_parse(page, address)
    pictures, links, lost = [], [], collections.Counter()
    at = {}
    for src, tiny in found:
        whole, why = web_resolve(web_attr_text(src, 600), address, base)
        if whole is None:
            lost.update([why] if why else [])
        elif whole in at:
            pictures[at[whole]][1] &= tiny
        elif len(at) < most_pictures:
            at[whole] = len(pictures)
            pictures.append([whole, tiny])
    for href in hrefs:
        href = web_attr_text(href, 600)
        if href[:1] == b"#":
            continue
        whole, why = web_resolve(href, address, base)
        if whole is None:
            lost.update([why] if why else [])
        else:
            links.append(whole.split(b"#", 1)[0])
    return pictures, links, lost


def cache_find(text, address, kind):
    """web_cache_find(): the line of an address in the index's bytes, halved until found.
    Returns (file, width, height) or None."""
    lo, hi = 0, len(text)
    while lo < hi:
        start = (lo + hi) // 2
        while start > lo and text[start - 1] != 0x0a:
            start -= 1
        end = text.find(b"\n", start, hi)
        end = hi if end < 0 else end
        if text[start] == 0x23:
            lo = end + 1
            continue
        at, tabs = start, 0
        while at < end and tabs < 4:
            tabs += text[at] == 9
            at += 1
        line, head = text[at:end], address[:end - at]
        order = (head > line) - (head < line) or int(len(address) > len(line))
        if not order:
            fields = text[start + 2:end].split()
            if text[start] != kind or len(fields) < 3:
                return None
            return fields[0][:63], int(fields[1]), int(fields[2])
        if order < 0:
            hi = start
        else:
            lo = end + 1
    return None


# ---- addresses ----

def usable(whole):
    """An address as text, or None and why it cannot be a line of the list."""
    try:
        return whole.decode("utf-8"), ""
    except UnicodeDecodeError:
        return None, "not UTF-8"


def plain(address):
    """The address the host is asked for: ask_part() of nxweb.c sends it without its fragment."""
    return address.split("#", 1)[0]


def wire(address):
    """The address as this tool sends it: no fragment, odd bytes escaped."""
    address = address.split("#", 1)[0]
    scheme, sep, rest = address.partition("://")
    host, slash, tail = rest.partition("/")
    if "?" in host:
        host, _, query = host.partition("?")
        path, has_query = "", True
    else:
        path, mark, query = tail.partition("?")
        has_query = bool(mark)
    kept = []
    for part in path.split("/"):
        if part == "..":
            if kept:
                kept.pop()
        elif part != ".":
            kept.append(part)
    if path.endswith(("/.", "/..")) or path in (".", ".."):
        kept.append("")
    safe = "/:@!$&'()*+,;=%~[]-._"
    out = scheme + sep + host + "/" + urllib.parse.quote("/".join(kept), safe=safe)
    return out + ("?" + urllib.parse.quote(query, safe=safe + "?") if has_query else "")


def host_of(address):
    rest = address.partition("://")[2]
    return re.split(r"[/?#]", rest, maxsplit=1)[0].lower()


STALE = (
    ("hash", re.compile(r"(?<![0-9A-Za-z])(?=[0-9a-f]*\d)(?=[0-9a-f]*[a-f])[0-9a-f]{8,}(?![0-9A-Za-z])")),
    ("hash", re.compile(r"(?<![0-9A-Za-z])(?=[0-9A-Za-z]*\d)(?=[0-9A-Za-z]*[a-z])(?=[0-9A-Za-z]*[A-Z])[0-9A-Za-z]{16,}(?![0-9A-Za-z])")),
    ("version", re.compile(r"(?:[?&](?:v|ver|version|rev|t|ts|cb|_|hash|h|m)=)|(?:[/@_-]v?\d+\.\d+\.\d+)")),
    ("version", re.compile(r"\?[0-9a-f.]+$")),
    ("date", re.compile(r"(?<!\d)(?:19|20)\d\d[/_-]?[01]\d(?:[/_-]?[0-3]\d)?(?!\d)")),
)


def stale(address):
    """What in an address looks as if it will change with the next release of the site."""
    tail = address.partition("://")[2].partition("/")[2]
    for label, pattern in STALE:
        if pattern.search(tail):
            return label
    return ""


ITEM = re.compile(r"\d{5,}|[?&](?:id|p|item|story|article|sid|tid|tag)=|/[^/?]*(?:[-_][^/?_-]+){3,}/?$|"
                  r"/(?=[0-9a-f]*\d)[0-9a-f]{6,}\.\w+$|/users?/|/u/|/~[^/]+/?$|/domains?/")
VISIT = re.compile(r"avatar|gravatar|thumb|userpic|profile|/users?/|/u/|upload|photo|comics?/|ichef|resiz|crop|"
                   r"\d{2,4}x\d{2,4}|[?&](?:w|h|width|height|size|s)=\d", re.I)
FURNITURE = re.compile(r"logo|icon|static|assets?/|sprite|badge|button|banner|header|footer|brand", re.I)
NEWS = re.compile(r"news|today|current|random|newest|live|of the day|every time|most read", re.I)


def item(address):
    """What in a page's address says it is one item of a changing list (a story, a post)."""
    tail = address.partition("://")[2].partition("/")[2]
    return stale(address) or ("item" if ITEM.search("/" + tail) else "")


def site_of(address):
    return ".".join(host_of(address).split(".")[-2:])


def stable_part(sites, own, linked, lines):
    """The lines likely to be right in a month, and which listed pages are changing fronts.
    A front changes if its name says so or a third of its own site's links are items."""
    steady, fronts = set(), []
    for (address, name), page, mine in zip(sites, own, linked):
        near = [a for a in page.links if site_of(a) == site_of(address)]
        front = bool(NEWS.search(name)) or (len(near) >= 6 and 3 * sum(1 for a in near if item(a)) >= len(near))
        fronts.append(front)
        steady.add(("P", address))
        pages = [page]
        for p in mine if not front else ():
            if p.good and not item(p.address):
                steady.add(("P", p.address))
                pages.append(p)
        for p in pages:
            steady.update(("S", plain(a)) for a in p.sheets if not item(plain(a)))
            # a changing front's pictures are its stories': only what is named as furniture stays
            steady.update(("I", plain(a)) for a in p.pictures if not item(plain(a)) and not VISIT.search(plain(a))
                          and (not front or FURNITURE.search(plain(a))))
    return [line for line in lines if line in steady], fronts


# ---- fetching ----

class Reply:
    def __init__(self, status=0, kind="", body=b"", error="", cut=False):
        self.status, self.kind, self.body, self.error, self.cut = status, kind, body, error, cut

    @property
    def ok(self):
        return self.status in (200, 206) and not self.error


def fetch(address, limit, accept="*/*", ranged=False):
    """One GET with one retry: at most `limit` bytes of the answer, its transport coding undone."""
    headers = {"User-Agent": AGENT, "Accept": accept, "Accept-Language": "en", "Accept-Encoding": "identity"}
    if ranged:
        headers["Range"] = "bytes=0-%d" % (limit - 1)
    reply = Reply(error="not tried")
    for attempt in range(2):
        if time.monotonic() > deadline:
            return Reply(error="out of time")
        try:
            request = urllib.request.Request(wire(address), headers=headers)
            with urllib.request.urlopen(request, timeout=15) as r:
                parts, size = [], 0
                while size <= limit and time.monotonic() < deadline + 10:
                    part = r.read(min(65536, limit + 1 - size))
                    if not part:
                        break
                    parts.append(part)
                    size += len(part)
                body = b"".join(parts)
                coding = (r.headers.get("Content-Encoding") or "").lower()
                if coding in ("gzip", "x-gzip", "deflate"):
                    body = zlib.decompressobj(47).decompress(body, limit + 1)
                elif coding == "br":
                    import brotli
                    body = brotli.decompress(body)
                return Reply(r.status, (r.headers.get("Content-Type") or "").lower(), body[:limit], cut=len(body) > limit)
        except urllib.error.HTTPError as e:
            reply = Reply(e.code, error="HTTP %d" % e.code)
            if e.code not in (429, 500, 502, 503, 504):
                return reply
            time.sleep(3.0 if e.code == 429 else 1.0)
        except Exception as e:
            reply = Reply(error=("%s: %s" % (type(e).__name__, e))[:90])
    return reply


def run_jobs(jobs):
    """Runs (host, function) jobs on the workers, at most PER_HOST of a host at a time."""
    results = [None] * len(jobs)
    queues = collections.OrderedDict()
    for index, (host, _) in enumerate(jobs):
        queues.setdefault(host, collections.deque()).append(index)
    hosts, active, state = list(queues), collections.Counter(), {"left": len(jobs), "turn": 0}
    cond = threading.Condition()

    def take():
        with cond:
            while state["left"]:
                for k in range(len(hosts)):
                    host = hosts[(state["turn"] + k) % len(hosts)]
                    if queues[host] and active[host] < PER_HOST:
                        active[host] += 1
                        state["left"] -= 1
                        state["turn"] += k + 1
                        return queues[host].popleft()
                cond.wait()
            return None

    def work():
        while True:
            index = take()
            if index is None:
                return
            try:
                results[index] = jobs[index][1]()
            except Exception as e:
                results[index] = Reply(error=("%s: %s" % (type(e).__name__, e))[:90])
            with cond:
                active[jobs[index][0]] -= 1
                cond.notify_all()

    threads = [threading.Thread(target=work, daemon=True) for _ in range(min(WORKERS, len(jobs)))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return results


memo = {}       # (what, the address as sent) -> Reply


def fetch_all(addresses, what, function):
    """Fetches each address once (two that differ by a fragment are one fetch)."""
    jobs, keys = [], []
    for address in addresses:
        key = (what, wire(address))
        if key not in memo and key not in keys:
            keys.append(key)
            jobs.append((host_of(address), lambda a=address: function(a)))
    for key, reply in zip(keys, run_jobs(jobs)):
        memo[key] = reply


def got(what, address):
    return memo.get((what, wire(address))) or Reply(error="not fetched")


# ---- pictures ----

def magic(body):
    if body.startswith(b"\x89PNG"):
        return "png"
    if body.startswith(b"\xff\xd8\xff"):
        return "jpeg"
    if body.startswith(b"GIF8"):
        return "gif"
    if body.startswith(b"RIFF") and body[8:12] == b"WEBP":
        return "webp"
    head = body[:300].lstrip().lower()
    if head.startswith(b"<svg") or (head.startswith(b"<?xml") and b"<svg" in body[:2000].lower()):
        return "svg"
    return ""


def probe(address):
    """A picture's first bytes: its kind by the answer's Content-Type and its size if it shows."""
    reply = fetch(address, PROBE, accept="image/png,image/jpeg,*/*;q=0.5", ranged=True)
    reply.size = None
    reply.form = magic(reply.body) if reply.ok else ""
    if "svg" in reply.kind:
        reply.form = "svg"
    if reply.ok and Image is not None and reply.form != "svg":
        try:
            reply.size = Image.open(io.BytesIO(reply.body)).size
        except Exception:
            pass
    reply.body = b""
    return reply


def stored(size):
    """The size a picture is kept at: no wider than WIDEST."""
    w, h = size
    return (WIDEST, max(1, round(h * WIDEST / w))) if w > WIDEST else (w, h)


def as_ppm(address):
    """Fetches a picture whole and makes the PPM of it: (bytes, w, h), or why not."""
    reply = fetch(address, PICTURE_READ, accept="image/png,image/jpeg,*/*;q=0.5")
    if not reply.ok or reply.cut:
        return reply.error or ("HTTP %d" % reply.status if not reply.ok else "over 8 MB")
    body = reply.body
    if "svg" in reply.kind or magic(body) == "svg":
        if cairosvg is None:
            return "SVG"
        body = cairosvg.svg2png(bytestring=body, unsafe=False)
    try:
        picture = Image.open(io.BytesIO(body))
        if picture.size[0] < 2 or picture.size[1] < 2:
            return "under 2 x 2"
        if picture.size[0] * picture.size[1] > 40000000:
            return "too many pixels"
        picture = picture.convert("RGBA")
        white = Image.new("RGBA", picture.size, (255, 255, 255, 255))
        white.alpha_composite(picture)
        picture = white.convert("RGB")
        if picture.size != stored(picture.size):
            picture = picture.resize(stored(picture.size), Image.LANCZOS)
    except Exception as e:
        return ("not decoded: %s" % e)[:80]
    w, h = picture.size
    if w < 2 or h < 2:
        return "under 2 x 2"
    data = b"P6\n%d %d\n255\n" % (w, h) + picture.tobytes()
    return "over 4 MB as a PPM" if len(data) > PPM_MOST else (data, w, h)


# ---- the crawl ----

class Page:
    def __init__(self, address):
        self.address = address
        self.body = b""
        self.good = False
        self.why = ""
        self.sheets, self.pictures, self.links = [], [], []     # as the browser looks them up
        self.small = set()      # pictures the page says are 2 x 2 or under: asked for, not drawn
        self.lost = collections.Counter()

    def read(self, args):
        reply = got("page", self.address)
        if not reply.ok:
            self.why = reply.error or "HTTP %d" % reply.status
            return
        if reply.kind and not any(t in reply.kind for t in ("html", "text/", "xml")):
            self.why = "not a page (%s)" % reply.kind.split(";")[0]
            return
        self.good = True
        self.body = reply.body[:PAGE_MOST]
        own = self.address.encode("utf-8")
        sheets, self.lost = web_find_sheets(self.body, own, args.sheets_per_page, args.view)
        pictures, links, lost = page_asks(self.body, own, args.pictures_per_page)
        self.lost.update(lost)
        small = {whole for whole, tiny in pictures if tiny}
        for found, into in ((sheets, self.sheets), ([whole for whole, _ in pictures], self.pictures), (links, self.links)):
            known = set()
            for whole in found:
                address, why = usable(whole)
                if address is None:
                    self.lost[why] += 1
                elif address not in known:
                    known.add(address)
                    into.append(address)
                    if into is self.pictures and whole in small:
                        self.small.add(address)


def is_web(address):
    return address.startswith(("https://", "http://"))


def crawl(args, sites):
    """The listed pages, up to links-per-site pages one click from each, their sheets, pictures."""
    def stage(name, began):
        print("  %-34s %6.1f s" % (name, time.monotonic() - began), flush=True)

    def resources(pages, name):
        began = time.monotonic()
        fetch_all([a for p in pages for a in p.sheets if is_web(a)], "sheet",
                  lambda a: fetch(a, SHEET_MOST, "text/css,*/*;q=0.5"))
        fetch_all([a for p in pages for a in p.pictures if is_web(a)], "probe", probe)
        stage(name, began)

    def pages_of(pages, name):
        began = time.monotonic()
        fetch_all([p.address for p in pages], "page", lambda a: fetch(a, PAGE_MOST, "text/html,*/*;q=0.8"))
        for page in pages:
            page.read(args)
        stage("%d %s" % (len(pages), name), began)

    # the listed pages and what they need come first: a site that tires of us has given those
    own = [Page(address) for address, _ in sites]
    pages_of(own, "listed pages")
    resources(own, "their sheets and pictures' starts")
    taken = {p.address for p in own}
    linked = []
    for page in own:
        mine = []
        for address in page.links:
            if len(mine) == args.links_per_site:
                break
            if address in taken or not is_web(address) or host_of(address) != host_of(page.address):
                continue
            taken.add(address)
            mine.append(Page(address))
        linked.append(mine)
    # a page from each site in turn, so that the time's end leaves no site without
    turn = [p for k in range(args.links_per_site) for mine in linked for p in mine[k:k + 1]]
    pages_of(turn, "pages one click away")
    resources(turn, "their sheets and pictures' starts")
    return own, linked


def picture_fate(address):
    """'' for a picture VRChat's loader takes, or why it is left out of the list."""
    reply = got("probe", address)
    if not reply.ok:
        return "failed: " + (reply.error or "HTTP %d" % reply.status)
    kind = reply.kind.split(";")[0].strip()
    if reply.form == "svg":
        return "SVG"
    if kind not in ("image/png", "image/jpeg", "image/jpg") or reply.form not in ("png", "jpeg", ""):
        return "not PNG or JPEG"
    if reply.size and max(reply.size) > 2048:
        return "over 2048 pixels a side"
    return ""


def list_links(sites, own, linked):
    """links.txt's lines in order, and a row of counts for each site."""
    lines, seen, rows, failures = [], set(), [], []
    lost = collections.Counter()

    def add(kind, address, row):
        address = plain(address)
        if (kind, address) in seen:
            return
        seen.add((kind, address))
        why = ""
        if not address.startswith("https://"):
            why = "not https"
        elif kind == "S":
            reply = got("sheet", address)
            why = "" if reply.ok and reply.body else "failed: " + (reply.error or "HTTP %d" % reply.status)
        elif kind == "I":
            why = picture_fate(address)
        if why.startswith("failed"):
            row["failures"] += 1
            failures.append("%s %s  %s" % (kind, address, why[8:]))
        elif why:
            row["dropped"] += 1
            lost[why] += 1
        else:
            row[kind] += 1
            lines.append((kind, address))

    for page in own:
        row = collections.Counter()
        rows.append(row)
        add("P", page.address, row)
        if not page.good:
            row["failures"] += 1
            failures.append("P %s  %s" % (page.address, page.why))
    for page, mine, row in zip(own, linked, rows):
        def resources(p):
            for why, count in p.lost.items():
                lost[why] += count
                row["dropped"] += count
            for address in p.sheets:
                add("S", address, row)
            for address in p.pictures:
                add("I", address, row)

        resources(page)
        for p in mine:
            if p.good:
                add("P", p.address, row)
            elif p.why.startswith("not a page"):
                row["dropped"] += 1
                lost["not a page"] += 1
            else:
                row["failures"] += 1
                failures.append("P %s  %s" % (p.address, p.why))
        for p in mine:
            if p.good:
                resources(p)
    return lines, rows, lost, failures


# ---- the cache ----

def build_cache(args, own, linked):
    """Writes the cache's files and index; returns what it says of itself."""
    os.makedirs(args.cache, exist_ok=True)
    for name in os.listdir(args.cache):
        if re.fullmatch(r"webc-\d+\.(css|ppm|html)", name) or name == "web-cache.idx":
            os.remove(os.path.join(args.cache, name))
    files, entries, notes = {}, {}, collections.Counter()
    state = {"next": 1, "spent": 0, "pages": 0}

    def keep(kind, address, data, suffix, w=0, h=0, counted=True):
        """One file for what an address holds; another address of the same file is a line more."""
        key = wire(address)
        if key not in files:
            if counted and state["spent"] + len(data) > args.budget:
                notes["left out by the budget"] += 1
                return False
            name = "webc-%04d.%s" % (state["next"], suffix)
            state["next"] += 1
            with open(os.path.join(args.cache, name), "wb") as f:
                f.write(data)
            state["spent" if counted else "pages"] += len(data)
            files[key] = (name, w, h)
        entries.setdefault(address, (kind,) + files[key])
        return True

    def sheets(pages):
        for p in pages:
            for address in p.sheets:
                reply = got("sheet", address)
                if address in entries or not is_web(address):
                    continue
                if not reply.ok or not reply.body:
                    notes["sheets not fetched"] += 1
                else:
                    # (the browser reads SHEET_MOST bytes of a longer one, from a file as from the host)
                    notes["sheets cut to %d bytes" % SHEET_MOST] += reply.cut
                    keep("S", address, reply.body, "css")

    def pictures(pages):
        if Image is None:
            return
        wanted, sized, blind = [], [], []
        for p in pages:
            for address in p.pictures:
                if address not in drawn:
                    notes["pictures 2 x 2 or under by their attributes: not kept"] += address not in entries
                    entries.setdefault(address, None)
                elif is_web(address) and address not in entries and address not in wanted:
                    wanted.append(address)
        for address in wanted:
            reply = got("probe", address)
            if not reply.ok:
                notes["pictures not fetched"] += 1
            elif reply.form == "svg" and cairosvg is None:
                notes["SVG pictures skipped (no rasteriser)"] += 1
            elif reply.size:
                w, h = stored(reply.size)
                sized.append((w * h * 3 + 20, address))
            else:
                blind.append(address)
        # the smallest first, as many as the budget could hold; then some whose size is not known
        room, chosen = args.budget - state["spent"], []
        for size, address in sorted(sized):
            if size > room:
                notes["left out by the budget"] += 1
                continue
            room -= size
            chosen.append(address)
        chosen += blind[:60] if room > 0 else []
        fetch_all(chosen, "ppm", as_ppm)
        made = []
        for address in chosen:
            result = memo.get(("ppm", wire(address)))
            if isinstance(result, tuple):
                made.append((len(result[0]), address, result))
            else:
                why = result.error if isinstance(result, Reply) else str(result)
                notes["SVG pictures skipped (no rasteriser)" if why == "SVG" else "pictures skipped: " + why.split(":")[0]] += 1
        for _, address, (data, w, h) in sorted(made, key=lambda m: m[:2]):
            keep("I", address, data, "ppm", w, h)

    every = own + [p for mine in linked for p in mine]
    drawn = {a for p in every for a in p.pictures if a not in p.small}
    if args.pages:
        for p in every:
            if p.good:
                keep("P", p.address, p.body, "html", counted=False)
    first, rest = own, [p for mine in linked for p in mine if p.good]
    for pages in (first, rest):
        sheets(pages)
        pictures(pages)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    # "# web-cache 1 <date>", then kind<TAB>file<TAB>width<TAB>height<TAB>address a line, in the
    # order strcmp() gives the addresses: web_cache_find() halves the file's bytes
    rows = sorted((address.encode("utf-8"), entry) for address, entry in entries.items() if entry)
    text = ("# web-cache 1 %s\n" % stamp).encode()
    for address, (kind, name, w, h) in rows:
        text += ("%s\t%s\t%d\t%d\t" % (kind, name, w, h)).encode() + address + b"\n"
    with open(os.path.join(args.cache, "web-cache.idx"), "wb") as f:
        f.write(text)
    state["found"] = sum(1 for address, (kind, name, w, h) in rows
                         if cache_find(text, address, ord(kind)) == (name.encode(), w, h))
    kinds = collections.Counter(entry[0] for _, entry in rows)
    return state, kinds, len(files), notes


# ---- main ----

def main():
    global deadline
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sites", default=os.path.join(HERE, "sites.txt"))
    ap.add_argument("--links", default=os.path.join(ROOT, "build", "web", "links.txt"))
    ap.add_argument("--cache", help="directory for the ROM's cache (none is written without)")
    ap.add_argument("--pages", action="store_true", help="the cache holds the pages' HTML too")
    ap.add_argument("--budget", type=int, default=8000000, help="bytes of sheets and pictures in the cache")
    ap.add_argument("--links-per-site", type=int, default=40)
    ap.add_argument("--stable", help="the part of the list likely to be right in a month (default: "
                    "the list's name with -stable, build/web/links-stable.txt)")
    ap.add_argument("--sheets-per-page", type=int, default=WEB_SHEETS)
    ap.add_argument("--pictures-per-page", type=int, default=MAX_PICTURES)
    ap.add_argument("--view", type=int, default=570, help="the page area's width, for media queries")
    ap.add_argument("--time", type=float, default=200, help="seconds after which nothing more is fetched")
    ap.add_argument("--all-failures", action="store_true", help="list every failure, not the first 30")
    args = ap.parse_args()
    args.stable = args.stable or "-stable".join(os.path.splitext(args.links))
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")
    began = time.monotonic()
    deadline = began + args.time

    sites = []
    with open(args.sites, encoding="utf-8") as f:
        for line in f:
            address, _, name = line.rstrip("\r\n").partition("\t")
            if address and not address.startswith("#"):
                sites.append((address, name))
    print("%d sites; Pillow %s, SVG %s" % (len(sites), "yes" if Image else "NO (no sizes, no PPM)",
                                           "yes" if cairosvg else "no rasteriser"), flush=True)
    own, linked = crawl(args, sites)
    lines, rows, lost, failures = list_links(sites, own, linked)

    os.makedirs(os.path.dirname(os.path.abspath(args.links)), exist_ok=True)
    with open(args.links, "w", encoding="utf-8", newline="\n") as f:
        for kind, address in lines:
            f.write("%s\t%s\n" % (kind, address))

    steady, fronts = stable_part(sites, own, linked, lines)
    os.makedirs(os.path.dirname(os.path.abspath(args.stable)), exist_ok=True)
    with open(args.stable, "w", encoding="utf-8", newline="\n") as f:
        for kind, address in steady:
            f.write("%s\t%s\n" % (kind, address))

    print("\n%-58s %5s %6s %8s %7s %8s" % ("site (* a changing front)", "pages", "sheets", "pictures", "dropped", "failures"))
    total = collections.Counter()
    for (address, _), row, front in zip(sites, rows, fronts):
        total.update(row)
        print("%-58s %5d %6d %8d %7d %8d" % (("* " if front else "  ") + address[:56], row["P"], row["S"], row["I"], row["dropped"], row["failures"]))
    print("%-58s %5d %6d %8d %7d %8d" % ("total", total["P"], total["S"], total["I"], total["dropped"], total["failures"]))
    print("\n%d lines in %s (P %d, S %d, I %d)" % (len(lines), args.links, total["P"], total["S"], total["I"]))
    kinds = collections.Counter(kind for kind, _ in steady)
    print("%d lines in %s (P %d, S %d, I %d)" % (len(steady), args.stable, kinds["P"], kinds["S"], kinds["I"]))
    print("pictures are listed whether or not a sheet hides them: what display:none takes is not known here")
    if lost:
        print("dropped: " + ", ".join("%d %s" % (count, why) for why, count in lost.most_common()))
    for kind, name in (("S", "sheets"), ("I", "pictures")):
        marks = collections.Counter(stale(address) for k, address in lines if k == kind)
        count = sum(marks.values())
        print("%s that look as if their address will change: %d of %d (hash %d, version %d, date %d)" %
              (name, count - marks[""], count, marks["hash"], marks["version"], marks["date"]))
    if failures:
        reasons = collections.Counter(line.rsplit("  ", 1)[1] for line in failures)
        print("\nfailures by reason: " + ", ".join("%d %s" % (count, why) for why, count in reasons.most_common(8)))
        shown = failures if args.all_failures else failures[:30]
        print("%d failures%s:" % (len(failures), "" if len(shown) == len(failures) else ", the first %d" % len(shown)))
        for line in shown:
            print("  " + line)

    if args.cache:
        state, kinds, count, notes = build_cache(args, own, linked)
        print("\ncache %s: %d files, %d index lines (P %d, S %d, I %d)" %
              (args.cache, count, sum(kinds.values()), kinds["P"], kinds["S"], kinds["I"]))
        print("  sheets and pictures %d bytes of %d, pages %d bytes" % (state["spent"], args.budget, state["pages"]))
        print("  web_cache_find()'s search finds %d of the index's %d lines" % (state["found"], sum(kinds.values())))
        for why, n in (notes - collections.Counter()).most_common():
            print("  %d %s" % (n, why))
    if time.monotonic() > deadline:
        print("\nTHE TIME RAN OUT: what was not fetched by then is among the failures")
    print("\nelapsed %.1f s" % (time.monotonic() - began))


if __name__ == "__main__":
    main()
