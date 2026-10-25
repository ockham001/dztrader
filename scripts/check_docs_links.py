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
CONTRACT_DIR = "docs/frame_contracts/"
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HEADING_RE = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$")
NUMBERED_HEADING_RE = re.compile(r"^#{2,6}\s+(\d+(?:\.\d+)*)[.．、]?\s")
SKIP_PREFIXES = ("http://", "https://", "mailto:")
# 契约内 §N 引用的三种实际写法（《帧契约：标题》§N / 契约 <文件名> §N / 总则 §N）；
# 裸 §N 按「本契约编号标题或总则编号标题」校验。见 docs/frame_contracts/general.md §11.2。
CONTRACT_REF_RES = (
    ("title", re.compile(r"《帧契约：([^》]+)》\s*§(\d+(?:\.\d+)*)")),
    ("stem", re.compile(r"(?<!《帧契约：)契约\s+([A-Za-z0-9_.-]+)\s*§(\d+(?:\.\d+)*)")),
    ("stemonly", re.compile(r"(?<![\w-])([a-z][a-z0-9-]+)\s+§(\d+(?:\.\d+)*)")),
    ("general", re.compile(r"总则\s*§(\d+(?:\.\d+)*)")),
)
# 指向本地归档设计文档（docs/specs/、未入库）的 §N 引用：无法验证，跳过裸 §N 校验。
EXTERNAL_SECTION_PREFIXES = ("设计", "方案", "计划")
BARE_SECTION_RE = re.compile(r"§(\d+(?:\.\d+)*)")


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


def numbered_headings(text):
    """Section numbers declared by numbered headings, e.g. {"2", "2.8", "6.1.1"}."""
    nums = set()
    for line in text.splitlines():
        m = NUMBERED_HEADING_RE.match(line)
        if m:
            nums.add(m.group(1))
    return nums


def contract_index(root):
    """Map contract titles/file stems to relative paths, and collect numbered headings."""
    idx = {"titles": {}, "stems": {}, "headings": {}}
    contract_dir = os.path.join(root, CONTRACT_DIR)
    if not os.path.isdir(contract_dir):
        return idx
    for name in sorted(os.listdir(contract_dir)):
        if not name.endswith(".md") or name == "README.md":
            continue
        rel = CONTRACT_DIR + name
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        idx["stems"][name[:-3]] = rel
        first = text.split("\n", 1)[0].strip()
        if first.startswith("# ") and "：" in first:
            idx["titles"][first.split("：", 1)[1].strip()] = rel
        idx["headings"][rel] = numbered_headings("\n".join(strip_code_fences(text)))
    return idx


def check_contract_refs(files, root, idx):
    """Validate §N references in docs/frame_contracts/*.md against numbered headings."""
    errors = []
    general_rel = CONTRACT_DIR + "general.md"
    general_nums = idx["headings"].get(general_rel, set())
    for rel in files:
        if not rel.startswith(CONTRACT_DIR) or rel.endswith("README.md"):
            continue
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
            text = "\n".join(strip_code_fences(fh.read()))
        own_nums = idx["headings"].get(rel, set())
        masked = list(text)
        for kind, pattern in CONTRACT_REF_RES:
            # 每类匹配后立即遮蔽已识别的 span，避免同一引用被后续规则重复报告。
            for m in pattern.finditer("".join(masked)):
                if kind == "general":
                    raw, num = None, m.group(1)
                    target = general_rel
                else:
                    raw, num = m.group(1), m.group(2)
                    target = idx["titles"].get(raw) or idx["stems"].get(raw)
                if kind == "stemonly" and target is None:
                    continue  # 不是已知契约名（例如正文提到某个缩写），不当作契约引用
                if target is None:
                    errors.append("%s:%d: unknown contract in §N ref -> %s"
                                  % (rel, text.count("\n", 0, m.start()) + 1, m.group(0)))
                    continue
                if num not in idx["headings"].get(target, set()):
                    hint = " (target has no numbered headings)" if not idx["headings"].get(target) else ""
                    errors.append("%s:%d: missing contract section -> %s%s"
                                  % (rel, text.count("\n", 0, m.start()) + 1, m.group(0), hint))
                masked[m.start():m.end()] = " " * (m.end() - m.start())
        bare_text = "".join(masked)
        for m in BARE_SECTION_RE.finditer(bare_text):
            num = m.group(1)
            if num in own_nums or num in general_nums:
                continue
            prefix = bare_text[max(0, m.start() - 4):m.start()].rstrip()
            if prefix.endswith(EXTERNAL_SECTION_PREFIXES):
                continue  # 指向本地归档设计文档的引用，不在本检查范围
            errors.append("%s:%d: missing section -> §%s"
                          % (rel, bare_text.count("\n", 0, m.start()) + 1, num))
    return errors


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

    contract_errors = check_contract_refs(files, root, contract_index(root))
    errors.extend(contract_errors)

    for err in errors:
        print(err)
    if errors:
        print(f"FAIL: {len(errors)} broken link(s)/contract ref(s)")
        return 1
    print(f"OK: {len(files)} markdown files scanned, no broken links")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
