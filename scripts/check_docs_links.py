#!/usr/bin/env python3
"""Check Markdown internal links: relative-path existence, heading anchors,
backticked repo-path existence, and backticked code-symbol existence.

Usage: python scripts/check_docs_links.py [files...]
       python scripts/check_docs_links.py --selftest   # 检查器自测（临时目录，不碰工作区）
Default file set: tracked *.md (git ls-files). Exit 1 on any broken link.

Skipped: absolute URLs (http/https/mailto) and links into local-only archives
(docs/plans/, docs/specs/, docs/superpowers/) which are not version-controlled.

符号检查（issue #191）：活跃文档里反引号包裹的 snake_case 标识符必须是代码里存在的
符号。复盘 #158-#161 发现「文档仍列已删除函数」（#161 第 2 轮审查 HARD）反复出现；
路径检查只能覆盖文件名，覆盖不到符号名。
"""

import os
import re
import subprocess
import sys
import tempfile
import urllib.parse

LOCAL_ARCHIVES = ("docs/plans/", "docs/specs/", "docs/superpowers/")
CONTRACT_DIR = "docs/frame_contracts/"
# Backticked repo paths in active docs must resolve. Scope mirrors check_ledger.py's
# "active documents" set: frozen local archives and ADRs are historical records and
# may legitimately name files that have since moved or been deleted.
ACTIVE_DOC_ROOTS = ("docs/components/", "docs/frame_contracts/", "docs/agents/")
ACTIVE_DOC_FILES = ("README.md", "GLOSSARY.md", "docs/README.md",
                    "docs/development-checklist.md", "docs/unit-test-standard.md")
REPO_TOP_DIRS = ("libs/", "apps/", "strategies/", "scripts/", "docs/", "cmake/",
                 ".github/", "third_party/", "tools/")
BACKTICK_PATH_RE = re.compile(
    r"`([A-Za-z0-9_./-]+\.(?:h|cpp|py|md|json|sh|ps1|txt|map|cmake|yml|yaml))`")
# 反引号里的 snake_case 标识符（函数/字段/配置键/工具名）。纯全大写、驼峰、带点路径不算。
BACKTICK_SYMBOL_RE = re.compile(r"`([a-z][a-z0-9_]{3,})`")
# 代码 token 索引的扫描范围（环境里可查的产物如 build/、node_modules/ 不扫；
# third_party 是 vendored 外部代码，扫进来会掩盖本仓符号缺失）。
SYMBOL_SCAN_ROOTS = ("libs", "apps", "strategies", "scripts", "cmake", ".github")
SYMBOL_SCAN_ROOT_FILES = ("CMakeLists.txt", "CMakePresets.json", "conanfile.py", "package.json")
SYMBOL_SCAN_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".c", ".py", ".sh", ".ps1", ".js", ".ts",
                        ".tsx", ".mjs", ".cmake", ".json", ".yml", ".yaml", ".toml", ".txt")
SYMBOL_SCAN_SKIP_DIRS = {"node_modules", "dist", "build", "install", ".cache", ".git",
                         "third_party", ".worktrees"}
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# 显式白名单：文档有意引用、但代码里查不到的「环境/工单/计划」名词。
# 判据与 check_backticked_paths 一致——只有「环境里能查到，或明确是计划/外部名词」才豁免；
# 已实现后又删掉的符号**不进白名单**（那正是要抓的漂移）。
SYMBOL_ALLOWLIST = {
    # CMake targets / 构建产物 / npm scripts（在 CMakePresets.json 或 package.json 里可查）
    "add_custom_command", "cmake_minimum_required", "copy_directory",
    "dz_on_md_started", "dz_preload_event", "dz_preload_md", "dz_wait_for",
    "dztest_support", "dz_shm_perf_bench", "dz_smoke_test",
    "dzmd_ctp_options", "ctp_2", "ctp_options", "frame_types_test", "alert_e2e",
    "stg_demo_test", "dztrader_sdk", "dztrader_sdk_static",
    # 计划中的基准项/文档示例（§10 基准清单）
    "bench_bar_close_dual_ma", "bench_bar_tick_daily", "bench_bar_tick_intraday",
    "bench_relocate", "bench_series_read", "bench_tick_update", "bench_warmup_replay",
    # 示例数据/平台遥测字段（文档示例，不是本仓符号）
    "bip_buffer", "ios_in_progress", "iwlwifi_1",
    # GitHub / DSH / issue 字段与工具名（含 MCP 外部工具名：AGENTS §六 引用 codegraph_explore，
    # 它是宿主提供的工具而非本仓符号）
    "blocked_by", "node_id", "package_id", "run_in_background", "wait_agent",
    "codegraph_explore",
    # 历史示例：review-checklist 用「已删除的 watch_path」说明漂移类 finding
    "watch_path",
}
# 成组豁免（前缀）：基准项、SDK target 家族。
SYMBOL_ALLOWLIST_PREFIXES = ("bench_", "dztrader_sdk_")


def is_active_doc(rel):
    """True when rel is an active (non-archived) document worth drift-checking."""
    return rel.startswith(ACTIVE_DOC_ROOTS) or rel in ACTIVE_DOC_FILES
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


def tracked_repo_paths(root):
    """All tracked file paths, for backticked-path drift detection."""
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root,
                         capture_output=True, check=True).stdout
    return {p for p in out.decode().split("\0") if p}


def check_backticked_paths(files, root, tracked):
    """Flag backticked repo paths in active docs that no longer exist.

    Doc drift recurs in essentially every review round here, and the dominant shape is
    a `path/to/file` inside a code span surviving the file's rename or deletion -- a
    bare backticked path is not a link, so nothing above catches it. Only repo-shaped
    paths (starting with a known top-level dir) are checked, which keeps prose such as
    `config.json` out.
    """
    errors = []
    for rel in files:
        if not is_active_doc(rel):
            continue
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
            lines = strip_code_fences(fh.read())
        for lineno, line in enumerate(lines, 1):
            for tok in BACKTICK_PATH_RE.findall(line):
                candidate = tok.lstrip("./")
                if "..." in candidate or candidate.startswith(LOCAL_ARCHIVES):
                    continue
                if not candidate.startswith(REPO_TOP_DIRS):
                    continue  # prose like `config.json`
                if candidate in tracked:
                    continue
                if any(t.endswith("/" + candidate) for t in tracked):
                    continue
                errors.append(f"{rel}:{lineno}: backticked path does not exist -> {tok}")
    return errors


def code_token_index(root):
    """Collect every identifier appearing in code/build/config text under root."""
    tokens = set()
    for name in SYMBOL_SCAN_ROOT_FILES:
        path = os.path.join(root, name)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                tokens.update(TOKEN_RE.findall(fh.read()))
        except OSError:
            continue
    for scan_root in SYMBOL_SCAN_ROOTS:
        base_dir = os.path.join(root, scan_root)
        if not os.path.isdir(base_dir):
            continue
        for dirpath, dirnames, filenames in os.walk(base_dir):
            dirnames[:] = [d for d in dirnames if d not in SYMBOL_SCAN_SKIP_DIRS]
            for filename in filenames:
                if not filename.endswith(SYMBOL_SCAN_SUFFIXES):
                    continue
                try:
                    with open(os.path.join(dirpath, filename), encoding="utf-8",
                              errors="replace") as fh:
                        tokens.update(TOKEN_RE.findall(fh.read()))
                except OSError:
                    continue
    return tokens


def check_backticked_symbols(files, root):
    """Flag backticked snake_case identifiers in active docs that code does not define."""
    code_tokens = code_token_index(root)
    errors = []
    for rel in files:
        if not is_active_doc(rel):
            continue
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
            lines = strip_code_fences(fh.read())
        for lineno, line in enumerate(lines, 1):
            for tok in BACKTICK_SYMBOL_RE.findall(line):
                if "_" not in tok or tok.endswith("_") or tok.startswith("_"):
                    continue
                if tok in SYMBOL_ALLOWLIST or tok.startswith(SYMBOL_ALLOWLIST_PREFIXES):
                    continue
                if tok not in code_tokens:
                    errors.append(f"{rel}:{lineno}: backticked symbol not found in code -> {tok}")
    return errors


def selftest():
    """Self-test for the symbol check on a throwaway tree (worktree untouched)."""
    with tempfile.TemporaryDirectory(prefix="dz_docs_links_selftest_") as tmp:
        os.makedirs(os.path.join(tmp, "libs", "demo"))
        with open(os.path.join(tmp, "libs", "demo", "sample.h"), "w", encoding="utf-8") as fh:
            fh.write("struct Sample { int field_name = 0; };\nvoid real_symbol();\n")
        os.makedirs(os.path.join(tmp, "docs", "components"))
        doc = os.path.join(tmp, "docs", "components", "sample.md")
        with open(doc, "w", encoding="utf-8") as fh:
            fh.write("\n".join([
                "`real_symbol` 存在 → 通过；`field_name` 存在 → 通过。",
                "`dztrader_sdk` 白名单 → 通过。",
                "`ghost_symbol` 不存在 → 必须被抓到。",
                "```",
                "`fenced_ghost_symbol` 在代码块里 → 忽略。",
                "```",
                "",
            ]))
        errors = check_backticked_symbols(["docs/components/sample.md"], tmp)
        if len(errors) != 1 or "ghost_symbol" not in errors[0] or "fenced" in errors[0]:
            print("selftest FAIL: expected exactly one ghost_symbol error, got:", errors)
            return 1
    print("selftest OK: symbol check detects missing symbols, honors allowlist and code fences")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
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
    errors.extend(check_backticked_paths(files, root, tracked_repo_paths(root)))
    errors.extend(check_backticked_symbols(files, root))

    for err in errors:
        print(err)
    if errors:
        print(f"FAIL: {len(errors)} broken link(s)/contract ref(s)/stale path(s)/stale symbol(s)")
        return 1
    print(f"OK: {len(files)} markdown files scanned, no broken links/paths/symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
