"""Render complete and split owner manuals. Requires: pip install pymupdf.

The renderer supports the Markdown constructs used by this manual: headings,
paragraphs, links, emphasis, code fences, lists and tables.
"""

from pathlib import Path
import base64
import html
import mimetypes
import re

import pymupdf


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "USER_MANUAL.md"
REPOSITORY = "https://github.com/arleybls/micropicodrive-ng-firmware/blob/main/"


def inline(text):
    tokens = []

    def token(value):
        tokens.append(value)
        return f"\x00{len(tokens) - 1}\x00"

    text = re.sub(r"`([^`]+)`", lambda m: token("<code>" + html.escape(m[1]) + "</code>"), text)

    def link(match):
        label, target = match.groups()
        if not target.startswith(("https://", "http://", "#")):
            path, separator, fragment = target.partition("#")
            relative = (SOURCE.parent / path).resolve().relative_to(ROOT).as_posix()
            target = REPOSITORY + relative + (separator + fragment)
        return token(f'<a href="{html.escape(target, quote=True)}">{inline(label)}</a>')

    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, text)
    text = html.escape(text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    # Link labels can contain an earlier protected code span.
    while "\x00" in text:
        text = re.sub(r"\x00(\d+)\x00", lambda m: tokens[int(m[1])], text)
    return text


def render_markdown(source):
    lines = source.splitlines()
    output = []
    index = 0
    list_type = None

    def close_list():
        nonlocal list_type
        if list_type:
            output.append(f"</{list_type}>")
            list_type = None

    while index < len(lines):
        line = lines[index]
        stripped = line.strip()
        if not stripped:
            index += 1
            continue
        if stripped.startswith("```"):
            close_list()
            block = []
            index += 1
            while index < len(lines) and not lines[index].strip().startswith("```"):
                block.append(lines[index].strip() if line.startswith(" ") else lines[index])
                index += 1
            output.append("<pre>" + html.escape("\n".join(block)) + "</pre>")
            index += 1
            continue
        picture = re.fullmatch(r"!\[([^]]*)\]\(([^)]+)\)", stripped)
        if picture:
            close_list()
            description, target = picture.groups()
            path = (SOURCE.parent / target).resolve()
            mime = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
            encoded = base64.b64encode(path.read_bytes()).decode("ascii")
            output.append(
                f'<figure><img src="data:{mime};base64,{encoded}" alt="{html.escape(description, quote=True)}">'
                f'<figcaption>{inline(description)}</figcaption></figure>'
            )
            index += 1
            continue
        heading = re.match(r"^(#{1,6}) (.+)$", line)
        if heading:
            close_list()
            level, title = len(heading[1]), heading[2]
            anchor = re.sub(r"[^\w\- ]", "", title.lower()).replace(" ", "-")
            output.append(f'<h{level} id="{anchor}">{inline(title)}</h{level}>')
            index += 1
            continue
        if stripped.startswith("|"):
            close_list()
            output.append("<table>")
            first = True
            while index < len(lines) and lines[index].strip().startswith("|"):
                cells = [cell.strip() for cell in lines[index].strip().strip("|").split("|")]
                if not all(re.fullmatch(r":?-+:?", cell) for cell in cells):
                    tag = "th" if first else "td"
                    output.append("<tr>" + "".join(f"<{tag}>{inline(cell)}</{tag}>" for cell in cells) + "</tr>")
                    first = False
                index += 1
            output.append("</table>")
            continue
        item = re.match(r"^(?:([-])|(\d+)\.) (.*)$", line)
        if item:
            kind = "ul" if item[1] else "ol"
            if list_type != kind:
                close_list()
                start = f' start="{item[2]}"' if kind == "ol" else ""
                output.append(f"<{kind}{start}>")
                list_type = kind
            paragraph = [item[3]]
            index += 1
            while index < len(lines) and lines[index].startswith("   ") and lines[index].strip() and not lines[index].strip().startswith(("```", "|")):
                paragraph.append(lines[index].strip())
                index += 1
            output.append("<li>" + inline(" ".join(paragraph)) + "</li>")
            continue
        close_list()
        paragraph = [stripped]
        index += 1
        while index < len(lines) and lines[index].strip() and not re.match(r"^(?:#|\||```|- |\d+\. )", lines[index]):
            paragraph.append(lines[index].strip())
            index += 1
        output.append("<p>" + inline(" ".join(paragraph)) + "</p>")
    close_list()
    return "\n".join(output)


CSS = """
body { font-family: sans-serif; font-size: 10pt; line-height: 1.35; color: #202b35; }
h1 { font-size: 27pt; color: #153c57; margin-bottom: 18pt; }
h2 { font-size: 18pt; color: #153c57; margin-top: 22pt; margin-bottom: 9pt; page-break-after: avoid; }
h3 { font-size: 12pt; color: #153c57; margin-top: 15pt; margin-bottom: 6pt; page-break-after: avoid; }
p { margin-top: 0; margin-bottom: 8pt; }
a { color: #135878; text-decoration: none; }
code { font-family: monospace; font-size: 9pt; }
pre { font-family: monospace; font-size: 9pt; padding: 9pt; }
table { border-collapse: collapse; width: 100%; font-size: 8.5pt; margin: 8pt 0 12pt; page-break-inside: avoid; }
th { font-weight: bold; color: #153c57; }
td, th { border: 0.5pt solid #b9c9d3; padding: 5pt; vertical-align: top; }
li { margin-bottom: 5pt; }
figure { margin: 10pt auto 12pt; text-align: center; page-break-inside: avoid; }
figure img { max-width: 78%; max-height: 260pt; }
figcaption { font-size: 8pt; color: #52616b; margin-top: 4pt; }
"""


def sections(source):
    matches = list(re.finditer(r"^## (.+)$", source, re.M))
    result = {"_front": source[:matches[0].start()]}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(source)
        result[match.group(1)] = source[match.start():end]
    return result


def edition_source(source, edition):
    if edition == "complete":
        return source
    parts = sections(source)
    if edition == "hardware":
        body = parts["The two parts and firmware versions"] + parts["Installing the mainboard in the QL"]
        body = re.sub(r"\]\(#([^)]+)\)", r"](USER_MANUAL.md#\1)", body)
        return "# MicroPicoDrive NG hardware installation manual\n\n" + \
            "This edition contains the physical installation and optional motor guidance from the owner's manual. " \
            "The photographs show the current NG boards and motor module.\n\n" + body
    excluded = {"Contents", "Installing the mainboard in the QL"}
    body = "".join(value for name, value in parts.items() if name != "_front" and name not in excluded)
    return "# MicroPicoDrive NG firmware manual\n\n" + \
        "This edition covers SD card preparation, everyday use, System Tools, Bluetooth, firmware updates and troubleshooting.\n\n" + body


def build(source, edition, destination, footer_title):
    story = pymupdf.Story(render_markdown(edition_source(source, edition)), user_css=CSS)
    paper = pymupdf.paper_rect("a4")
    area = pymupdf.Rect(44, 44, paper.width - 44, paper.height - 46)
    document = story.write_with_links(lambda *_: (paper, area, None))
    for number, page in enumerate(document, 1):
        page.draw_line((44, paper.height - 34), (paper.width - 44, paper.height - 34), color=(0.7, 0.76, 0.8), width=0.5)
        page.insert_text((44, paper.height - 22), f"MicroPicoDrive NG | {footer_title} | v2.10.1", fontsize=8, color=(0.35, 0.4, 0.45))
        page.insert_text((paper.width - 75, paper.height - 22), f"{number} / {len(document)}", fontsize=8)
    document.set_metadata({"title": "MicroPicoDrive NG owner's manual", "subject": "Installation, SD cards and System Tools", "author": "MicroPicoDrive NG project"})
    document.save(destination, garbage=4, deflate=True)
    print(f"Created {destination} ({len(document)} pages)")


def main():
    source = SOURCE.read_text(encoding="utf-8")
    editions = [
        ("complete", SOURCE.with_suffix(".pdf"), "Owner's manual"),
        ("firmware", SOURCE.parent / "FIRMWARE_MANUAL.pdf", "Firmware manual"),
        ("hardware", SOURCE.parent / "HARDWARE_INSTALLATION_MANUAL.pdf", "Hardware installation"),
    ]
    for edition, destination, title in editions:
        build(source, edition, destination, title)


if __name__ == "__main__":
    main()
