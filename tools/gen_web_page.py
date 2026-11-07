#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Evergo-Mainline
"""Embed web/index.html into main/web_page.h as a C string constant.

The firmware serves the page from flash; keeping the HTML here and generating
the header means the page is edited in exactly one place.
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "web" / "index.html"
DST = ROOT / "main" / "web_page.h"

HEADER = "/* generated from web/index.html by tools/gen_web_page.py - do not edit */\n#pragma once\n\nstatic const char PAGE_HTML[] =\n"


def escape(line: str) -> str:
    return (
        line.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\t", "\\t")
        .replace("\n", "\\n")
    )


def main() -> None:
    html = SRC.read_text(encoding="utf-8").replace("\r\n", "\n")
    lines = html.split("\n")
    body = []
    for i, line in enumerate(lines):
        text = line + ("\n" if i < len(lines) - 1 else "")
        if not text and i == len(lines) - 1:
            continue  # trailing newline of the source file, nothing to add
        body.append('    "' + escape(text) + '"')
    out = HEADER + "\n".join(body) + ";\n"
    DST.write_text(out, encoding="utf-8", newline="\n")
    print(f"web_page.h: {len(out)} bytes from {SRC.name} ({len(html)} bytes)")


if __name__ == "__main__":
    main()
