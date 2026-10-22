"""Build the Synx documentation as static HTML.

Usage: python docs/site/build.py [output-folder]

The default output folder is dist/docs-site. Open index.html in a browser;
no server is needed. Only the Python standard library is used. The
converter supports the Markdown that the documentation uses: headings,
paragraphs, ordered and unordered lists, tables, fenced code, inline code,
links, emphasis, and strong text.
"""

from html import escape
from pathlib import Path
import re
import shutil
import sys

DOCS = Path(__file__).resolve().parent.parent
ROOT = DOCS.parent
STYLE = Path(__file__).resolve().parent / "style.css"

# Pages in the site, relative to docs/. Folders include every .md file.
SOURCES = [
    "*.md",
    "guide",
    "reference",
    "library",
]
# Files copied without change, relative to docs/.
COPIES = ["grammar.ebnf"]
NAV = [
    ("Home", "index.html"),
    ("Install", "guide/install.html"),
    ("Tutorial", "guide/tutorial/index.html"),
    ("Language", "reference/index.html"),
    ("Library", "library/index.html"),
    ("Commands", "guide/commands.html"),
]

KEYWORDS = set("""as async await break const continue dyn else enum extern false fn
for if impl import in loop match pub return self Self struct trait true unsafe
var while once send""".split())
TYPES = set("""bool byte short int long uint ulong usize float double char void str
String Vec Option Result Box HashMap Some None Ok Err""".split())
TOKEN = re.compile(r"""(?P<comment>//[^\n]*|/\*.*?\*/)
    |(?P<string>"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])+')
    |(?P<number>\b\d[\d_]*(?:\.\d+)?(?:[eE][+-]?\d+)?[fL]?\b|\b0[xb][0-9A-Fa-f_]+\b)
    |(?P<word>[A-Za-z_][A-Za-z0-9_]*)""", re.X | re.S)


def highlight(code):
    out, at = [], 0
    for m in TOKEN.finditer(code):
        out.append(escape(code[at:m.start()]))
        text = escape(m.group())
        kind = m.lastgroup
        if kind == "word":
            if m.group() in KEYWORDS:
                kind = "kw"
            elif m.group() in TYPES:
                kind = "ty"
            else:
                kind = None
        out.append(f'<span class="{kind}">{text}</span>' if kind else text)
        at = m.end()
    out.append(escape(code[at:]))
    return "".join(out)


def slug(text, used):
    """GitHub heading anchors: lowercase, punctuation removed, spaces to
    hyphens, and -1, -2, ... for repeated headings on one page."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text).lower()
    text = re.sub(r"[^\w\- ]", "", text).replace(" ", "-")
    base, n = text, 0
    while text in used:
        n += 1
        text = f"{base}-{n}"
    used.add(text)
    return text


def link_target(target):
    if re.match(r"[a-z]+:", target):
        return target
    path, _, anchor = target.partition("#")
    if path.endswith(".md"):
        path = path[:-3] + ".html"
    return path + ("#" + anchor if anchor else "")


INLINE = re.compile(r"(`[^`]+`|\[[^\]]+\]\([^)\s]+\)|\*\*[^*]+\*\*|\*[^*\s][^*]*\*)")


def inline(text):
    parts = []
    for piece in INLINE.split(text):
        if not piece:
            continue
        if piece.startswith("`") and piece.endswith("`") and len(piece) > 1:
            parts.append("<code>" + escape(piece[1:-1]) + "</code>")
        elif piece.startswith("[") and piece.endswith(")"):
            m = re.fullmatch(r"\[([^\]]+)\]\(([^)\s]+)\)", piece)
            parts.append(f'<a href="{escape(link_target(m[2]), quote=True)}">{inline(m[1])}</a>')
        elif piece.startswith("**") and piece.endswith("**") and len(piece) > 4:
            parts.append("<strong>" + inline(piece[2:-2]) + "</strong>")
        elif piece.startswith("*") and piece.endswith("*") and len(piece) > 2:
            parts.append("<em>" + inline(piece[1:-1]) + "</em>")
        else:
            parts.append(escape(piece).replace("\\|", "|"))
    return "".join(parts)


def table(rows):
    # A cell can hold "\|", which GitHub shows as "|" even inside code.
    split = [re.split(r"(?<!\\)\|", r.strip().strip("|")) for r in rows]
    cells = [[c.strip().replace(r"\|", "|") for c in row] for row in split]
    head, body = cells[0], cells[2:]
    html = ["<table><thead><tr>"]
    html += [f"<th>{inline(c)}</th>" for c in head]
    html.append("</tr></thead><tbody>")
    for row in body:
        html.append("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in row) + "</tr>")
    html.append("</tbody></table>")
    return "".join(html)


def render(markdown):
    """Returns (title, body html, list of (level, id, text) headings)."""
    lines = markdown.splitlines()
    out, toc, title = [], [], ""
    used = set()
    para, i = [], 0
    list_stack = []  # (kind, indent)

    def flush_para():
        if para:
            out.append("<p>" + inline(" ".join(para)) + "</p>")
            para.clear()

    def close_lists(indent=-1):
        while list_stack and list_stack[-1][1] > indent:
            kind, _ = list_stack.pop()
            out.append(f"</li></{kind}>")

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        indent = len(line) - len(line.lstrip(" "))
        if stripped.startswith("```"):
            flush_para()
            lang = stripped[3:].strip()
            code = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i][indent:] if lines[i][:indent].strip() == "" else lines[i])
                i += 1
            body = "\n".join(code)
            body = highlight(body) if lang == "synx" else escape(body)
            out.append(f'<pre class="{lang or "text"}"><code>{body}</code></pre>')
            i += 1
            continue
        if not stripped:
            flush_para()
            if list_stack:
                nxt = next((l for l in lines[i + 1:] if l.strip()), "")
                nind = len(nxt) - len(nxt.lstrip(" "))
                if nind == 0 and not re.match(r"\s*(?:[-*]|\d+\.)\s", nxt):
                    close_lists()
            i += 1
            continue
        heading = re.match(r"(#{1,6})\s+(.*)", line)
        if heading and indent == 0:
            flush_para()
            close_lists()
            level, text = len(heading[1]), heading[2].strip()
            ident = slug(text, used)
            if level == 1 and not title:
                title = re.sub(r"`", "", text)
            toc.append((level, ident, text))
            out.append(f'<h{level} id="{ident}">{inline(text)}</h{level}>')
            i += 1
            continue
        if stripped.startswith("|") and i + 1 < len(lines) and re.match(r"\s*\|[\s:|-]+\|\s*$", lines[i + 1]):
            flush_para()
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(lines[i])
                i += 1
            out.append(table(rows))
            continue
        item = re.match(r"(\s*)([-*]|\d+\.)\s+(.*)", line)
        if item:
            flush_para()
            kind = "ol" if item[2][0].isdigit() else "ul"
            if not list_stack or indent > list_stack[-1][1]:
                start = f' start="{item[2][:-1]}"' if kind == "ol" and item[2] != "1." else ""
                out.append(f"<{kind}{start}><li>")
                list_stack.append((kind, indent))
            else:
                close_lists(indent)
                out.append("</li><li>")
            para.append(item[3])
            i += 1
            continue
        para.append(stripped)
        i += 1
    flush_para()
    close_lists()
    return title, "\n".join(out), toc


def page(title, body, toc, depth):
    up = "../" * depth
    nav = " ".join(f'<a href="{up}{href}">{name}</a>' for name, href in NAV)
    sections = [t for t in toc if t[0] == 2]
    side = ""
    if len(sections) > 2:
        side = '<nav class="toc"><p>On this page</p><ul>' + "".join(
            f'<li><a href="#{i}">{inline(t)}</a></li>' for _, i, t in sections) + "</ul></nav>"
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{escape(title)} - Synx</title>
<link rel="stylesheet" href="{up}style.css">
</head>
<body>
<header><a class="home" href="{up}index.html">Synx</a> <span class="version">0.1.0</span><nav>{nav}</nav></header>
<div class="layout">
<main>
{body}
</main>
{side}
</div>
</body>
</html>
"""


def sources():
    for entry in SOURCES:
        path = DOCS / entry
        if "*" in entry:
            yield from sorted(DOCS.glob(entry))
        elif path.is_dir():
            yield from sorted(path.rglob("*.md"))
        elif path.exists():
            yield path


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist" / "docs-site"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    count = 0
    for src in sources():
        rel = src.relative_to(DOCS).with_suffix(".html")
        title, body, toc = render(src.read_text(encoding="utf-8"))
        target = out_dir / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(page(title or rel.stem, body, toc, len(rel.parts) - 1), encoding="utf-8")
        count += 1
    shutil.copyfile(STYLE, out_dir / "style.css")
    for name in COPIES:
        shutil.copyfile(DOCS / name, out_dir / name)
    broken = check_links(out_dir)
    print(f"{count} pages written to {out_dir}")
    for page_path, href in broken:
        print(f"broken link in {page_path.relative_to(out_dir)}: {href}")
    return 1 if broken else 0


def check_links(out_dir):
    """Returns (page, href) for each relative link whose file or anchor is missing."""
    ids, broken = {}, []
    pages = sorted(out_dir.rglob("*.html"))
    for p in pages:
        ids[p.resolve()] = set(re.findall(r'id="([^"]+)"', p.read_text(encoding="utf-8")))
    for p in pages:
        for href in re.findall(r'href="([^"]+)"', p.read_text(encoding="utf-8")):
            if re.match(r"[a-z]+:", href) or href.endswith(".css"):
                continue
            path, _, anchor = href.partition("#")
            target = (p.parent / path).resolve() if path else p.resolve()
            if not target.is_relative_to(out_dir.resolve()):
                continue  # a source file outside docs/, such as selfhost/README.md
            if not target.exists() or (anchor and target.suffix == ".html" and anchor not in ids.get(target, set())):
                broken.append((p, href))
    return broken


if __name__ == "__main__":
    sys.exit(main())
