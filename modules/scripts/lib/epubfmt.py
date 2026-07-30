#
# EPUB package/navigation/content parsing, on top of epubzip.py (the ZIP
# container) and modxml.parse() (the XML tree)
# Copyright (c) 2026 8bitmcu
# License: MIT
#

import modxml
import epubzip

_CONTAINER_PATH = "META-INF/container.xml"

_BLOCK_TAGS = frozenset((
    "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6",
    "li", "blockquote", "pre", "tr",
))


class EpubFormatError(Exception):
    pass


def _find_first(node, tag):
    """ Depth-first search for the first element anywhere below (or at)
    `node` with the given tag name. """
    if node["tag"] == tag:
        return node
    for child in node["children"]:
        found = _find_first(child, tag)
        if found is not None:
            return found
    return None


def _find_all(node, tag, out=None):
    """ Depth-first collection of every element anywhere below (or at)
    `node` with the given tag name, in document order. """
    if out is None:
        out = []
    if node["tag"] == tag:
        out.append(node)
    for child in node["children"]:
        _find_all(child, tag, out)
    return out


def _direct_children(node, tag):
    return [c for c in node["children"] if c["tag"] == tag]


def _collect_text(node):
    """ Flattens all descendant text, ignoring nested tag structure --
    used for a link's display text, since inline formatting tags (em,
    strong, ...) aren't rendered by this text-mode reader anyway. """
    parts = [node["text"]]
    for child in node["children"]:
        parts.append(_collect_text(child))
    return "".join(parts)


def _unquote(s):
    """ Minimal percent-decoder for OPF/NCX/nav href values -- they're
    IRI-encoded per spec (e.g. spaces as %20) but zip entry names are
    stored raw, so hrefs need decoding before they'll match namelist(). """
    if "%" not in s:
        return s
    out = bytearray()
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == "%" and i + 2 < n:
            try:
                out.append(int(s[i + 1:i + 3], 16))
                i += 3
                continue
            except ValueError:
                pass
        out.append(ord(c))
        i += 1
    return out.decode("utf-8", "ignore")


def _dirname(path):
    idx = path.rfind("/")
    return path[:idx] if idx >= 0 else ""


def _normpath(path):
    """ Resolves '.' and '..' segments in a POSIX-style path -- pure
    string manipulation (these paths only ever refer to zip members,
    never the real filesystem). """
    parts = []
    for part in path.split("/"):
        if part == "" or part == ".":
            continue
        elif part == "..":
            if parts:
                parts.pop()
        else:
            parts.append(part)
    return "/".join(parts)


def _resolve(base_dir, href):
    """ Resolves an href found inside a file at `base_dir` into a
    zip-root-relative path, stripping any '#fragment'. """
    href = href.split("#", 1)[0]
    return _normpath(base_dir + "/" + href if base_dir else href)


def _walk_body(node, base_dir, segments):
    """ Recursively walks a chapter's XHTML body, appending (text,
    href_or_None) segments in document order -- matching bin/wiki.py's
    pager segment convention (link segments carry a target, plain
    segments carry None). Block-level elements emit a trailing '\\n';
    the pager handles all wrapping/reflow itself, so no other layout
    decisions happen here. """
    tag = node["tag"]

    if tag in ("script", "style"):
        return

    if tag == "img":
        alt = node["attrs"].get("alt", "").strip()
        segments.append(("[image: %s]\n" % alt if alt else "[image]\n", None))
        return

    if tag == "a" and "href" in node["attrs"]:
        text = _collect_text(node)
        if text.strip():
            segments.append((text, _resolve(base_dir, _unquote(node["attrs"]["href"]))))
        if tag in _BLOCK_TAGS:
            segments.append(("\n", None))
        return

    if node["text"].strip():
        segments.append((node["text"], None))

    for child in node["children"]:
        _walk_body(child, base_dir, segments)

    if tag in _BLOCK_TAGS:
        segments.append(("\n", None))


class EpubBook:
    def __init__(self, path):
        self.zip = epubzip.ZipReader(path)
        self.manifest = {}  # item id -> {"href": zip-root-relative path, "media_type": str}
        self.spine = []     # ordered list of manifest ids (reading order)
        self.toc = []       # ordered list of (title, href)
        self.title = "Untitled"

        try:
            opf_path = self._resolve_opf_path()
            self._parse_opf(opf_path)
            self._parse_toc()
        except Exception:
            self.zip.close()
            raise

    def close(self):
        self.zip.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _resolve_opf_path(self):
        data = self.zip.read(_CONTAINER_PATH).decode("utf-8", "ignore")
        tree = modxml.parse(data)
        if tree is None:
            raise EpubFormatError("unreadable %s" % _CONTAINER_PATH)
        rootfile = _find_first(tree, "rootfile")
        full_path = rootfile["attrs"].get("full-path") if rootfile else None
        if not full_path:
            raise EpubFormatError("no rootfile in %s" % _CONTAINER_PATH)
        return _unquote(full_path)

    def _parse_opf(self, opf_path):
        self.opf_path = opf_path
        opf_dir = _dirname(opf_path)

        data = self.zip.read(opf_path).decode("utf-8", "ignore")
        tree = modxml.parse(data)
        if tree is None:
            raise EpubFormatError("unreadable OPF package document %r" % opf_path)

        metadata_el = _find_first(tree, "metadata")
        if metadata_el is not None:
            title_el = _find_first(metadata_el, "dc:title") or _find_first(metadata_el, "title")
            if title_el is not None and title_el["text"].strip():
                self.title = title_el["text"].strip()

        manifest_el = _find_first(tree, "manifest")
        nav_href = None
        ncx_href_by_type = None
        if manifest_el is not None:
            for item in _direct_children(manifest_el, "item"):
                item_id = item["attrs"].get("id")
                href = item["attrs"].get("href")
                if not item_id or not href:
                    continue
                media_type = item["attrs"].get("media-type", "")
                properties = item["attrs"].get("properties", "")
                resolved = _resolve(opf_dir, _unquote(href))

                self.manifest[item_id] = {"href": resolved, "media_type": media_type}

                if "nav" in properties.split():
                    nav_href = resolved
                if media_type == "application/x-dtbncx+xml":
                    ncx_href_by_type = resolved

        spine_el = _find_first(tree, "spine")
        toc_ncx_id = None
        if spine_el is not None:
            toc_ncx_id = spine_el["attrs"].get("toc")
            for itemref in _direct_children(spine_el, "itemref"):
                idref = itemref["attrs"].get("idref")
                linear = itemref["attrs"].get("linear", "yes")
                if idref and idref in self.manifest and linear != "no":
                    self.spine.append(idref)

        self._nav_href = nav_href
        ncx_href_by_id = self.manifest[toc_ncx_id]["href"] if toc_ncx_id in self.manifest else None
        self._ncx_href = ncx_href_by_id or ncx_href_by_type

    def _parse_toc(self):
        # EPUB3 nav document takes priority; EPUB2 NCX is the fallback.
        if self._nav_href is not None:
            self.toc = self._parse_nav_toc(self._nav_href)
        elif self._ncx_href is not None:
            self.toc = self._parse_ncx_toc(self._ncx_href)

    def _parse_nav_toc(self, nav_href):
        data = self.zip.read(nav_href).decode("utf-8", "ignore")
        tree = modxml.parse(data)
        if tree is None:
            return []
        base_dir = _dirname(nav_href)

        navs = _find_all(tree, "nav")
        toc_scope = None
        for nav in navs:
            if nav["attrs"].get("epub:type") == "toc":
                toc_scope = nav
                break
        if toc_scope is None and navs:
            toc_scope = navs[0]
        if toc_scope is None:
            return []

        toc = []
        for a in _find_all(toc_scope, "a"):
            href = a["attrs"].get("href")
            if not href:
                continue
            title = _collect_text(a).strip() or href
            toc.append((title, _resolve(base_dir, _unquote(href))))
        return toc

    def _parse_ncx_toc(self, ncx_href):
        data = self.zip.read(ncx_href).decode("utf-8", "ignore")
        tree = modxml.parse(data)
        if tree is None:
            return []
        base_dir = _dirname(ncx_href)

        toc = []
        for navpoint in _find_all(tree, "navPoint"):
            navlabels = _direct_children(navpoint, "navLabel")
            contents = _direct_children(navpoint, "content")
            if not navlabels or not contents:
                continue
            text_children = _direct_children(navlabels[0], "text")
            title = (text_children[0]["text"].strip() if text_children else "") or "Untitled"
            href = contents[0]["attrs"].get("src")
            if not href:
                continue
            toc.append((title, _resolve(base_dir, _unquote(href))))
        return toc

    def chapter_hrefs(self):
        """ Ordered (zip-root-relative) chapter paths, spine reading order. """
        return [self.manifest[idref]["href"] for idref in self.spine]

    def read_chapter_segments(self, href):
        """ Returns a list of (text, href_or_None) segments for the
        chapter at `href`, ready for tui.make_pager() -- see
        bin/wiki.py's _render_article() for the same convention. """
        try:
            raw = self.zip.read(href)
        except KeyError:
            return [("(missing chapter: %s)" % href, None)]

        xhtml = raw.decode("utf-8", "ignore")
        tree = modxml.parse(xhtml)
        if tree is None:
            return [("(unable to parse this chapter)", None)]

        body = _find_first(tree, "body") or tree
        segments = []
        _walk_body(body, _dirname(href), segments)
        if not segments:
            segments = [("(empty chapter)", None)]
        return segments
