#!/usr/bin/env python3
"""Check Markdown internal links: relative-path existence and heading anchors.

Usage: python scripts/check_docs_links.py [files...]
Default file set: tracked *.md (git ls-files). Exit 1 on any broken link.

Skipped: absolute URLs (http/https/mailto) and links into local-only archives
(docs/plans/, docs/specs/, docs/superpowers/) which are not version-controlled.
"""

import os
import re
import subprocess
import sys
import urllib.parse

LOCAL_ARCHIVES = ("docs/plans/", "docs/specs/", "docs/superpowers/")
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HEADING_RE = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$")
SKIP_PREFIXES = ("http://", "https://", "mailto:")


def strip_code_fences(text):
    """Blank out fenced code blocks so links inside them are ignored."""
    out, fence = [], False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fence = not fence
            out.append("")
            continue
        out.append("" if fence else line)
    return out


def slugify(text):
    """Approximate GitHub heading slug: lower, drop punctuation, spaces -> '-'."""
    text = text.strip().lower()
    text = re.sub(r"[`*]", "", text)
    text = re.sub(r"[^\w\s\u4e00-\u9fff-]", "", text, flags=re.UNICODE)
    text = re.sub(r"\s+", "-", text)
    return text


def heading_slugs(lines):
    return {slugify(m.group(1)) for line in lines for m in [HEADING_RE.match(line)] if m}


def main(argv):
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True, check=True).stdout.strip()
    if argv:
        files = argv
    else:
        out = subprocess.run(["git", "ls-files", "-z", "--", "*.md"],
                             capture_output=True, check=True).stdout
        files = [f for f in out.decode().split("\0") if f]

    errors = []
    slugs_cache = {}
    for rel in files:
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
            lines = strip_code_fences(fh.read())
        for lineno, line in enumerate(lines, 1):
            for target in LINK_RE.findall(line):
                if target.startswith(SKIP_PREFIXES) or target.startswith("<"):
                    continue
                raw_path, _, frag = target.partition("#")
                decoded = urllib.parse.unquote(raw_path)
                if decoded:
                    resolved = os.path.normpath(
                        os.path.join(os.path.dirname(rel), decoded)).replace(os.sep, "/")
                else:
                    resolved = rel
                resolved = resolved.lstrip("/")
                if resolved.startswith(LOCAL_ARCHIVES):
                    continue  # local-only archives: not verifiable in CI
                if not os.path.exists(os.path.join(root, resolved)):
                    errors.append(f"{rel}:{lineno}: missing target -> {target}")
                    continue
                if frag and resolved.endswith(".md"):
                    if resolved not in slugs_cache:
                        with open(os.path.join(root, resolved), encoding="utf-8",
                                  errors="replace") as fh:
                            slugs_cache[resolved] = heading_slugs(strip_code_fences(fh.read()))
                    if slugify(urllib.parse.unquote(frag)) not in slugs_cache[resolved]:
                        errors.append(f"{rel}:{lineno}: missing anchor -> {target}")

    for err in errors:
        print(err)
    if errors:
        print(f"FAIL: {len(errors)} broken link(s)")
        return 1
    print(f"OK: {len(files)} markdown files scanned, no broken links")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
