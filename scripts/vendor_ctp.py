#!/usr/bin/env python3
"""vendor_ctp.py — CTP 厂商发布包入库规范化与 vendored 记录校验

为什么需要（#166 取证留下的三个口子，见 #178）：
  1. 厂商发布包两侧头文件编码 / 行尾不同（Windows: GBK + CRLF；Linux: GBK + LF），
     入库前必须转成 UTF-8 + LF，否则 MSVC 报编码告警、Git diff 噪音大。
     这一转码原本是「手工另存」，升级时极易漏文件或只改一侧；
  2. `error.xml` 是本项目唯一内容有偏离的文本文件（XML 声明 `gb2312` 要改成 `utf-8`），
     手工处理时容易被忘记，导致解析器读中文 prompt 乱码；
  3. 逐文件 SHA-256 只写在人读的记录里，无法机械核对「仓库文件 = 记录」。

唯一记录真相是 `third_party/ctp/VENDORED.md`（版本 / 发布包哈希 / 逐文件哈希清单）。
本脚本是它唯一的生成与校验入口，清单不允许手工维护（校验规则 R1–R5 见该文档）。

  `--check`    只读校验（不联网、不需要发布包）：文件集合 / 逐文件哈希 / 文本编码 /
               `error.xml` 声明 / 记录字段；
  `--sync`     从厂商发布包 zip 重新生成 18 个文件，并回写记录（版本 / 包哈希 / 清单）；
  `--selftest` 临时目录注入已知违例（check 路径，精确断言命中规则集合）+ 合成发布包
               端到端跑 sync（含幂等），不触碰工作区。

规范化规则（`--sync` 的行为，与 VENDORED.md「规范化规则」一一对应）：
  1. 二进制逐字节复制；
  2. 文本 GB18030 → UTF-8，CRLF → LF；
  3. `error.xml` 的 XML 声明改为 `encoding="utf-8"`（唯一内容偏离）；
  4. 回写 VENDORED.md 的版本 / 包哈希 / 逐文件清单（对应「升级纪律」第 2 步）。

用法:
  python3 scripts/vendor_ctp.py --check
  python3 scripts/vendor_ctp.py --sync --zip <发布包路径>
  python3 scripts/vendor_ctp.py --selftest

退出码: 0=通过, 1=校验失败, 2=参数 / 环境错误
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import re
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path

# ---- 入库布局（VENDORED.md 清单的期望文件集合） ----------------------------

CTP_REL = Path("third_party/ctp")
DOC_REL = CTP_REL / "VENDORED.md"
PLATFORMS = ("linux", "windows")

LINUX_FILES = (
    "error.dtd",
    "error.xml",
    "ThostFtdcMdApi.h",
    "ThostFtdcTraderApi.h",
    "ThostFtdcUserApiDataType.h",
    "ThostFtdcUserApiStruct.h",
    "thostmduserapi_se.so",
    "thosttraderapi_se.so",
)
WINDOWS_FILES = (
    "error.dtd",
    "error.xml",
    "ThostFtdcMdApi.h",
    "ThostFtdcTraderApi.h",
    "ThostFtdcUserApiDataType.h",
    "ThostFtdcUserApiStruct.h",
    "thostmduserapi_se.dll",
    "thostmduserapi_se.lib",
    "thosttraderapi_se.dll",
    "thosttraderapi_se.lib",
)
EXPECTED_FILES = tuple(f"linux/{name}" for name in LINUX_FILES) + tuple(
    f"windows/{name}" for name in WINDOWS_FILES
)
TEXT_SUFFIXES = (".h", ".xml", ".dtd")

# ---- 发布包内来源（目标文件 → 目录键 / 包内文件名 / 是否文本） -------------
# 目录键：md_win / td_win 为 Windows 64 位 API 目录，md_linux / td_linux 为 Linux x64 目录。
SOURCE_PLAN: dict[str, tuple[str, str, bool]] = {
    "linux/error.dtd": ("md_linux", "error.dtd", True),
    "linux/error.xml": ("md_linux", "error.xml", True),
    "linux/ThostFtdcMdApi.h": ("md_linux", "ThostFtdcMdApi.h", True),
    "linux/ThostFtdcTraderApi.h": ("td_linux", "ThostFtdcTraderApi.h", True),
    "linux/ThostFtdcUserApiDataType.h": ("md_linux", "ThostFtdcUserApiDataType.h", True),
    "linux/ThostFtdcUserApiStruct.h": ("md_linux", "ThostFtdcUserApiStruct.h", True),
    "linux/thostmduserapi_se.so": ("md_linux", "thostmduserapi_se.so", False),
    "linux/thosttraderapi_se.so": ("td_linux", "thosttraderapi_se.so", False),
    "windows/error.dtd": ("md_win", "error.dtd", True),
    "windows/error.xml": ("md_win", "error.xml", True),
    "windows/ThostFtdcMdApi.h": ("md_win", "ThostFtdcMdApi.h", True),
    "windows/ThostFtdcTraderApi.h": ("td_win", "ThostFtdcTraderApi.h", True),
    "windows/ThostFtdcUserApiDataType.h": ("md_win", "ThostFtdcUserApiDataType.h", True),
    "windows/ThostFtdcUserApiStruct.h": ("md_win", "ThostFtdcUserApiStruct.h", True),
    "windows/thostmduserapi_se.dll": ("md_win", "thostmduserapi_se.dll", False),
    "windows/thostmduserapi_se.lib": ("md_win", "thostmduserapi_se.lib", False),
    "windows/thosttraderapi_se.dll": ("td_win", "thosttraderapi_se.dll", False),
    "windows/thosttraderapi_se.lib": ("td_win", "thosttraderapi_se.lib", False),
}
if set(SOURCE_PLAN) != set(EXPECTED_FILES):  # 单一文件集合定义，防止两处漂移
    raise RuntimeError("SOURCE_PLAN and EXPECTED_FILES disagree")  # pragma: no cover

# ---- VENDORED.md 的可机械解析字段 -----------------------------------------

MANIFEST_START = "<!-- vendor-ctp:manifest:start -->"
MANIFEST_END = "<!-- vendor-ctp:manifest:end -->"

DOC_VERSION_RE = re.compile(r"^\|\s*版本\s*\|\s*\*\*([0-9]+\.[0-9]+\.[0-9]+)\*\*\s*\|$", re.MULTILINE)
DOC_INNER_RE = re.compile(r"^\|\s*发布包内部版本\s*\|\s*`(v[^`]+)`\s*\|$", re.MULTILINE)
DOC_PKG_NAME_RE = re.compile(r"^\|\s*发布包文件\s*\|\s*`([^`]+)`\s*\|$", re.MULTILINE)
DOC_PKG_SHA_RE = re.compile(r"^\|\s*发布包 SHA-256\s*\|\s*`([0-9a-f]{64})`\s*\|$", re.MULTILINE)
MANIFEST_ROW_RE = re.compile(r"^\|\s*`(linux|windows)/([^`]+)`\s*\|\s*`([0-9a-f]{64})`\s*\|$", re.MULTILINE)

INNER_VERSION_SEARCH = re.compile(r"v([0-9]+\.[0-9]+\.[0-9]+)_([0-9]{8})")
XML_DECL_RE = re.compile(r'^<\?xml\s+version="1\.0"\s+encoding="[^"]+"\?>\s*$', re.MULTILINE)


class VendorError(Exception):
    """参数 / 环境错误（退出码 2），与「校验失败」（退出码 1）区分。"""


@dataclass
class Violation:
    rule: str
    message: str

    def render(self) -> str:
        return f"CTP-VENDOR {self.rule}: {self.message}"


# ---- 基础工具 --------------------------------------------------------------


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_text(relative: str) -> bool:
    return relative.endswith(TEXT_SUFFIXES)


def repo_root(explicit: str | None) -> Path:
    if explicit:
        root = Path(explicit).resolve()
        if not root.is_dir():
            raise VendorError(f"repository root not found: {root}")
        return root
    # 脚本位于 <root>/scripts/，向上一级即仓库根
    return Path(__file__).resolve().parent.parent


def read_doc(root: Path) -> str:
    doc = root / DOC_REL
    if not doc.is_file():
        raise VendorError(f"missing {DOC_REL.as_posix()} (run --sync first)")
    return doc.read_text(encoding="utf-8")


def manifest_span(doc: str) -> tuple[int, int] | None:
    start = doc.find(MANIFEST_START)
    end = doc.find(MANIFEST_END)
    if start < 0 or end < 0 or end < start:
        return None
    return start, end + len(MANIFEST_END)


def parse_manifest(doc: str) -> dict[str, str]:
    span = manifest_span(doc)
    if span is None:
        return {}
    rows: dict[str, str] = {}
    for platform, name, digest in MANIFEST_ROW_RE.findall(doc[span[0] : span[1]]):
        rows[f"{platform}/{name}"] = digest
    return rows


def actual_files(root: Path) -> set[str]:
    ctp = root / CTP_REL
    found: set[str] = set()
    for platform in PLATFORMS:
        base = ctp / platform
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.is_file():
                found.add(path.relative_to(ctp).as_posix())
    return found


def normalize_text(data: bytes, relative: str) -> bytes:
    """厂商文本 → 入库形态：GB18030 → UTF-8、CRLF → LF、error.xml 声明改 utf-8。"""
    try:
        text = data.decode("gb18030")
    except UnicodeDecodeError as exc:  # 发布包形态意外，宁可失败也不静默
        raise VendorError(f"cannot decode {relative} from the vendor package as gb18030: {exc}") from exc
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    if relative.endswith("error.xml"):
        text = XML_DECL_RE.sub('<?xml version="1.0" encoding="utf-8"?>', text, count=1)
    return text.encode("utf-8")


# ---- 校验（R1–R5，规则说明见 VENDORED.md「校验规则」） ---------------------


def check(root: Path) -> list[Violation]:
    violations: list[Violation] = []

    try:
        doc = read_doc(root)
    except VendorError as exc:
        return [Violation("R5", str(exc))]

    ctp = root / CTP_REL
    manifest = parse_manifest(doc)
    actual = actual_files(root)
    expected = set(EXPECTED_FILES)

    # R1 清单与仓库文件集合一致（多一个少一个都红）
    for name in sorted(expected - actual):
        violations.append(Violation("R1", f"missing file: {(CTP_REL / name).as_posix()}"))
    for name in sorted(actual - expected):
        violations.append(Violation("R1", f"unexpected file: {(CTP_REL / name).as_posix()}"))
    for name in sorted(set(manifest) - expected):
        violations.append(Violation("R1", f"manifest lists unknown file: {name}"))
    for name in sorted(expected - set(manifest)):
        violations.append(Violation("R1", f"manifest missing entry: {name}"))

    # R2 逐文件 SHA-256 与清单一致
    for name in sorted(expected & actual):
        recorded = manifest.get(name)
        if recorded is None:
            continue
        actual_sha = sha256_file(ctp / name)
        if actual_sha != recorded:
            violations.append(
                Violation("R2", f"sha256 mismatch: {name} (manifest={recorded[:12]}… actual={actual_sha[:12]}…)")
            )

    # R3 文本文件必须 UTF-8 无 BOM、无 CRLF
    for name in sorted(expected & actual):
        if not is_text(name):
            continue
        data = (ctp / name).read_bytes()
        if data.startswith(b"\xef\xbb\xbf"):
            violations.append(Violation("R3", f"utf-8 BOM present: {name}"))
        if b"\r" in data:
            violations.append(Violation("R3", f"CRLF/CR line ending: {name}"))
        try:
            data.decode("utf-8")
        except UnicodeDecodeError as exc:
            violations.append(Violation("R3", f"not valid utf-8: {name} ({exc.reason})"))

    # R4 error.xml 的 XML 声明必须是 utf-8（唯一内容偏离，见 VENDORED.md「已知偏离」）
    for name in ("linux/error.xml", "windows/error.xml"):
        target = ctp / name
        if not target.is_file():
            continue
        first_line = target.read_text(encoding="utf-8", errors="replace").splitlines()[:1]
        declaration = first_line[0] if first_line else ""
        if 'encoding="utf-8"' not in declaration:
            violations.append(Violation("R4", f"{name} declaration must be utf-8: {declaration!r}"))

    # R5 VENDORED.md 的可机械字段格式合法（清单区由 markers 界定）
    if manifest_span(doc) is None:
        violations.append(Violation("R5", "VENDORED.md manifest markers missing"))
    for regex, label in (
        (DOC_VERSION_RE, "版本"),
        (DOC_INNER_RE, "发布包内部版本"),
        (DOC_PKG_NAME_RE, "发布包文件"),
        (DOC_PKG_SHA_RE, "发布包 SHA-256"),
    ):
        if not regex.search(doc):
            violations.append(Violation("R5", f"VENDORED.md missing well-formed `{label}` row"))

    return violations


# ---- 发布包解压 ------------------------------------------------------------


def _safe_extract_zip(path: Path, target: Path) -> None:
    target = target.resolve()
    with zipfile.ZipFile(path) as archive:
        for member in archive.infolist():
            destination = (target / member.filename.replace("\\", "/")).resolve()
            if not destination.is_relative_to(target):
                raise VendorError(f"unsafe path in zip {path.name}: {member.filename}")
            if member.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as source, destination.open("wb") as output:
                output.write(source.read())


def _safe_extract_tar(path: Path, target: Path) -> None:
    with tarfile.open(path) as archive:
        archive.extractall(target, filter="data")


def extract_package(zip_path: Path, dest: Path) -> None:
    """递归解压厂商发布包（zip 套 zip，Linux 侧为 tar；跳过 chm 文档包）。"""
    root = dest / "pkg"
    root.mkdir(parents=True, exist_ok=True)
    _safe_extract_zip(zip_path, root)

    processed: set[Path] = set()
    for _ in range(4):  # 发布包最多 3 层（外层 zip → trader zip → api zip → tar）
        pending = [
            path
            for pattern in ("*.zip", "*.tar")
            for path in root.rglob(pattern)
            if path.is_file() and path not in processed and "chm" not in path.name.lower()
        ]
        if not pending:
            return
        for archive in pending:
            processed.add(archive)
            target = archive.parent / f"{archive.stem}__unpacked"
            if target.exists():
                continue
            if archive.suffix == ".zip":
                _safe_extract_zip(archive, target)
            else:
                _safe_extract_tar(archive, target)
    raise VendorError("vendor package nesting deeper than expected")


def find_unique_parent(root: Path, name: str, prefer: str) -> Path:
    """定位发布包内某个文件所在的唯一目录；同名的 32 / 64 位候选按 prefer 强制筛选。"""
    candidates = sorted({p.parent for p in root.rglob(name) if p.is_file()})
    if not candidates:
        raise VendorError(f"vendor package has no {name}")
    chosen = [p for p in candidates if prefer in p.as_posix()]
    if not chosen:
        listing = "\n  ".join(p.as_posix() for p in candidates)
        raise VendorError(f"vendor package has no {name} under a {prefer} directory; candidates:\n  {listing}")
    if len(chosen) > 1:
        listing = "\n  ".join(p.as_posix() for p in chosen)
        raise VendorError(f"ambiguous {name} directories:\n  {listing}")
    return chosen[0]


def derive_version(pkg_root: Path) -> tuple[str, str]:
    """从发布包内路径名推导内部版本（形如 v6.7.13_20260225_trader.zip），要求全局唯一。"""
    versions: set[tuple[str, str]] = set()
    for path in pkg_root.rglob("*"):
        found = INNER_VERSION_SEARCH.search(path.name)
        if found:
            versions.add((found.group(1), found.group(2)))
    if not versions:
        raise VendorError("cannot determine vendor inner version (vX.Y.Z_YYYYMMDD)")
    if len(versions) > 1:
        listing = ", ".join(f"v{version}_{day}" for version, day in sorted(versions))
        raise VendorError(f"ambiguous vendor inner version: {listing}")
    return versions.pop()


# ---- 从发布包同步 ----------------------------------------------------------


def _require_row(doc: str, regex: re.Pattern[str], label: str) -> None:
    if not regex.search(doc):
        raise VendorError(f"{DOC_REL.as_posix()} missing well-formed `{label}` row")


def sync(zip_path: Path, root: Path) -> None:
    if not zip_path.is_file():
        raise VendorError(f"vendor package not found: {zip_path}")

    # 先校验记录结构再动文件：记录不合法时不允许留下「写了一半」的入库目录
    doc = read_doc(root)
    _require_row(doc, DOC_PKG_SHA_RE, "发布包 SHA-256")
    _require_row(doc, DOC_VERSION_RE, "版本")
    _require_row(doc, DOC_INNER_RE, "发布包内部版本")
    _require_row(doc, DOC_PKG_NAME_RE, "发布包文件")
    if manifest_span(doc) is None:
        raise VendorError(f"{DOC_REL.as_posix()} manifest markers missing")

    sha_row = DOC_PKG_SHA_RE.search(doc)
    if sha_row is None:  # _require_row 已校验，这里防御未来重构
        raise VendorError(f"{DOC_REL.as_posix()} missing well-formed `发布包 SHA-256` row")
    recorded_sha = sha_row.group(1)
    actual_sha = sha256_file(zip_path)
    if recorded_sha != actual_sha:
        raise VendorError(
            "vendor package sha256 mismatch: "
            f"recorded={recorded_sha} actual={actual_sha}; "
            "升级发布包时先在 VENDORED.md 更新「发布包 SHA-256」，再运行 --sync"
        )

    with tempfile.TemporaryDirectory(prefix="vendor_ctp_") as tmp:
        extract_package(zip_path, Path(tmp))
        pkg_root = Path(tmp) / "pkg"
        version, build_date = derive_version(pkg_root)

        directories = {
            "md_win": find_unique_parent(pkg_root, "thostmduserapi_se.dll", prefer="64_se"),
            "td_win": find_unique_parent(pkg_root, "thosttraderapi_se.dll", prefer="64_se"),
            "md_linux": find_unique_parent(pkg_root, "thostmduserapi_se.so", prefer="linux64"),
            "td_linux": find_unique_parent(pkg_root, "thosttraderapi_se.so", prefer="linux64"),
        }
        missing = [
            (directories[directory] / name).as_posix()
            for directory, name, _text in SOURCE_PLAN.values()
            if not (directories[directory] / name).is_file()
        ]
        if missing:
            raise VendorError("vendor package missing files:\n  " + "\n  ".join(missing))

        # 全部转换在内存完成后再落盘（转换失败不留半成品）
        payload: dict[str, bytes] = {}
        for relative in EXPECTED_FILES:
            directory, name, text_file = SOURCE_PLAN[relative]
            source = directories[directory] / name
            payload[relative] = normalize_text(source.read_bytes(), relative) if text_file else source.read_bytes()

    ctp = root / CTP_REL
    for relative, data in payload.items():
        target = ctp / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    # 回写记录：版本 / 包哈希 / 逐文件清单
    manifest_lines = [MANIFEST_START, "| 文件 | SHA-256 |", "|---|---|"]
    for relative in EXPECTED_FILES:
        manifest_lines.append(f"| `{relative}` | `{sha256_file(ctp / relative)}` |")
    manifest_lines.append(MANIFEST_END)
    span = manifest_span(doc)
    if span is None:  # 前面已校验，这里防御未来重构
        raise VendorError(f"{DOC_REL.as_posix()} manifest markers missing")
    updated = doc[: span[0]] + "\n".join(manifest_lines) + doc[span[1] :]
    updated = re.sub(
        r"(?m)^\|\s*版本\s*\|\s*\*\*[0-9.]+\*\*\s*\|$",
        f"| 版本 | **{version}** |",
        updated,
        count=1,
    )
    updated = re.sub(
        r"(?m)^\|\s*发布包内部版本\s*\|\s*`[^`]+`\s*\|$",
        f"| 发布包内部版本 | `v{version}_{build_date}` |",
        updated,
        count=1,
    )
    updated = re.sub(
        r"(?m)^\|\s*发布包文件\s*\|\s*`[^`]+`\s*\|$",
        f"| 发布包文件 | `{zip_path.name}` |",
        updated,
        count=1,
    )
    updated = re.sub(
        r"(?m)^\|\s*发布包 SHA-256\s*\|\s*`[0-9a-f]{64}`\s*\|$",
        f"| 发布包 SHA-256 | `{actual_sha}` |",
        updated,
        count=1,
    )
    (root / DOC_REL).write_text(updated, encoding="utf-8")

    print(f"sync: release package {zip_path.name} -> v{version}_{build_date} (sha256 {actual_sha})")
    print(f"sync: wrote {len(EXPECTED_FILES)} files under {CTP_REL.as_posix()}/, manifest:")
    for relative in EXPECTED_FILES:
        print(f"| `{relative}` | `{sha256_file(ctp / relative)}` |")


# ---- 自测 ------------------------------------------------------------------


def _make_zip(entries: dict[str, bytes]) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return buffer.getvalue()


def _make_tar(entries: dict[str, bytes]) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as archive:
        for name, data in entries.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


_TEXT_HEADER = "// 合成头文件\n#define SYNTHETIC 1\n"
_ERR_DTD = '<?xml version="1.0" encoding="UTF-8"?>\n<!ELEMENT errors (error*)>\n'
_ERR_XML = (
    '<?xml version="1.0" encoding="gb2312"?>\n<errors>\n'
    '\t<error id="NONE" value="0" prompt="CTP:正确"/>\n</errors>\n'
)


def _text_bytes(text: str, platform: str) -> bytes:
    """合成发布包里的文本文件：Windows 为 GBK + CRLF，Linux 为 GBK + LF。"""
    encoded = text.encode("gb18030")
    return encoded.replace(b"\n", b"\r\n") if platform == "windows" else encoded


def _make_api_zip(api: str) -> bytes:
    """构造一个 API zip：Windows 64 位目录 + 32 位诱饵目录（同一 dll，内容不同）。"""
    if api == "md":
        api_header = "ThostFtdcMdApi.h"
        api_dll, api_lib, api_so = "thostmduserapi_se.dll", "thostmduserapi_se.lib", "thostmduserapi_se.so"
        win_dir = "20200101_mduserapi64_se_windows"
    else:
        api_header = "ThostFtdcTraderApi.h"
        api_dll, api_lib, api_so = "thosttraderapi_se.dll", "thosttraderapi_se.lib", "thosttraderapi_se.so"
        win_dir = "20200101_traderapi64_se_windows"
    decoy_dir = win_dir.replace("64_se", "_se")
    entries: dict[str, bytes] = {}
    for directory in (win_dir, decoy_dir):
        entries[f"{directory}/{api_dll}"] = f"{api}-windows-{directory}".encode()
        entries[f"{directory}/{api_lib}"] = f"{api}-windows-lib-{directory}".encode()
    for name, text in (
        (api_header, _TEXT_HEADER),
        ("ThostFtdcUserApiDataType.h", _TEXT_HEADER),
        ("ThostFtdcUserApiStruct.h", _TEXT_HEADER),
        ("error.xml", _ERR_XML),
        ("error.dtd", _ERR_DTD),
    ):
        entries[f"{win_dir}/{name}"] = _text_bytes(text, "windows")

    linux_root = f"v9.9.9_20990101_api/v9.9.9_20990101_api_{api}api_se_linux64"
    linux: dict[str, bytes] = {f"{linux_root}/{api_so}": f"{api}-linux".encode()}
    for name, text in (
        (api_header, _TEXT_HEADER),
        ("ThostFtdcUserApiDataType.h", _TEXT_HEADER),
        ("ThostFtdcUserApiStruct.h", _TEXT_HEADER),
        ("error.xml", _ERR_XML),
        ("error.dtd", _ERR_DTD),
    ):
        linux[f"{linux_root}/{name}"] = _text_bytes(text, "linux")
    entries[f"v9.9.9_20990101_api_{api}api_linux64.tar"] = _make_tar(linux)
    return _make_zip(entries)


def _write_synthetic_package(path: Path) -> None:
    """构造与真实发布包同构的合成包（外层 zip → trader zip → api zip → tar）。"""
    trader_zip = _make_zip(
        {
            "v9.9.9_20990101_mduserapi.zip": _make_api_zip("md"),
            "v9.9.9_20990101_traderapi.zip": _make_api_zip("trader"),
        }
    )
    path.write_bytes(_make_zip({"vendor_synthetic/v9.9.9_20990101_trader.zip": trader_zip}))


def _write_synthetic_tree(root: Path) -> None:
    """check 路径用的合成入库树：18 个文件 + 与之匹配的 VENDORED.md。"""
    ctp = root / CTP_REL
    for platform in PLATFORMS:
        (ctp / platform).mkdir(parents=True, exist_ok=True)
    for relative in EXPECTED_FILES:
        target = ctp / relative
        if relative.endswith("error.xml"):
            target.write_text('<?xml version="1.0" encoding="utf-8"?>\n<errors/>\n', encoding="utf-8")
        elif is_text(relative):
            target.write_text("// synthetic\n", encoding="utf-8")
        else:
            target.write_bytes(b"\x00CTP\x00")
    _write_doc_skeleton(root, "9" * 64, "6.7.13", "v6.7.13_20260225", "ctp-6.7.13.zip")


def _write_doc_skeleton(root: Path, pkg_sha: str, version: str, inner: str, pkg_name: str) -> None:
    ctp = root / CTP_REL
    ctp.mkdir(parents=True, exist_ok=True)
    lines = [MANIFEST_START, "| 文件 | SHA-256 |", "|---|---|"]
    for relative in EXPECTED_FILES:
        lines.append(f"| `{relative}` | `{sha256_file(ctp / relative)}` |")
    lines.append(MANIFEST_END)
    (root / DOC_REL).write_text(
        "# synthetic vendored record\n\n"
        "| 项 | 内容 |\n|---|---|\n"
        f"| 版本 | **{version}** |\n"
        f"| 发布包内部版本 | `{inner}` |\n"
        f"| 发布包文件 | `{pkg_name}` |\n"
        f"| 发布包 SHA-256 | `{pkg_sha}` |\n\n" + "\n".join(lines) + "\n",
        encoding="utf-8",
    )


def _snapshot(root: Path) -> dict[str, bytes]:
    """记录 third_party/ctp 下全部文件内容，用于幂等性断言。"""
    ctp = root / CTP_REL
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in sorted(ctp.rglob("*"))
        if path.is_file()
    }


def _check_selftest_cases() -> int:
    """check 路径：注入已知违例形态，断言**精确命中**的规则集合。"""
    failures = 0

    def expect(label: str, expected_rules: frozenset[str], mutate) -> None:
        nonlocal failures
        with tempfile.TemporaryDirectory(prefix="vendor_ctp_selftest_") as tmp:
            root = Path(tmp)
            _write_synthetic_tree(root)
            mutate(root)
            found = frozenset(v.rule for v in check(root))
            if found == expected_rules:
                print(f"selftest ok {label} -> {sorted(found)}")
            else:
                failures += 1
                print(f"selftest FAILED {label}: expected {sorted(expected_rules)}, got {sorted(found)}")

    def clean(root: Path) -> None:
        pass

    def drift(root: Path) -> None:
        (root / CTP_REL / "linux" / "ThostFtdcMdApi.h").write_text("// changed\n", encoding="utf-8")

    def crlf(root: Path) -> None:
        (root / CTP_REL / "linux" / "ThostFtdcTraderApi.h").write_bytes(b"// crlf\r\n")

    def bom(root: Path) -> None:
        (root / CTP_REL / "windows" / "ThostFtdcMdApi.h").write_bytes(b"\xef\xbb\xbf// bom\n")

    def declaration(root: Path) -> None:
        (root / CTP_REL / "linux" / "error.xml").write_text(
            '<?xml version="1.0" encoding="gb2312"?>\n<errors/>\n', encoding="utf-8"
        )

    def missing(root: Path) -> None:
        (root / CTP_REL / "windows" / "thostmduserapi_se.lib").unlink()

    def nested_extra(root: Path) -> None:
        extra = root / CTP_REL / "linux" / "extra" / "dup.h"
        extra.parent.mkdir(parents=True)
        extra.write_text("// extra\n", encoding="utf-8")

    def doc_field(root: Path) -> None:
        doc = (root / DOC_REL).read_text(encoding="utf-8")
        (root / DOC_REL).write_text(doc.replace("| 版本 | **6.7.13** |", "| 版本 | 6.7.13 |"), encoding="utf-8")

    # 文本字节变化必然连带 R2（哈希），这是预期规则集合，不是噪音
    expect("clean tree", frozenset(), clean)
    expect("sha256 drift", frozenset({"R2"}), drift)
    expect("crlf", frozenset({"R2", "R3"}), crlf)
    expect("utf-8 bom", frozenset({"R2", "R3"}), bom)
    expect("xml declaration", frozenset({"R2", "R4"}), declaration)
    expect("missing file", frozenset({"R1"}), missing)
    expect("nested extra file", frozenset({"R1"}), nested_extra)
    expect("doc field", frozenset({"R5"}), doc_field)
    return failures


def _sync_selftest_case() -> int:
    """sync 端到端：合成发布包 → 写 18 文件 + 回写记录；断言转换、筛选与幂等。"""
    failures = 0

    def fail(message: str) -> None:
        nonlocal failures
        failures += 1
        print(f"selftest FAILED sync: {message}")

    with tempfile.TemporaryDirectory(prefix="vendor_ctp_sync_selftest_") as tmp:
        root = Path(tmp)
        package = root / "release.zip"
        _write_synthetic_package(package)
        (root / CTP_REL).mkdir(parents=True)
        for relative in EXPECTED_FILES:  # 先放占位，保证记录可解析
            target = root / CTP_REL / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"stale")
        _write_doc_skeleton(root, sha256_file(package), "0.0.0", "v0.0.0_19700101", package.name)

        # 1) 第一次 sync：写入 + 回写记录
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            sync(package, root)
        output = buffer.getvalue()
        for probe in ("| `linux/thostmduserapi_se.so` | `", "| `windows/thostmduserapi_se.dll` | `"):
            if probe not in output:
                fail(f"sync did not print manifest row {probe!r}")

        windows_dll = (root / CTP_REL / "windows" / "thostmduserapi_se.dll").read_bytes()
        if windows_dll == b"md-windows-20200101_mduserapi_se_windows":
            fail("sync picked the 32-bit windows directory instead of *64_se*")
        linux_so = (root / CTP_REL / "linux" / "thostmduserapi_se.so").read_bytes()
        if linux_so != b"md-linux":
            fail(f"sync did not copy linux .so as-is: {linux_so!r}")
        windows_header = (root / CTP_REL / "windows" / "ThostFtdcMdApi.h").read_bytes()
        if b"\r" in windows_header or windows_header != _TEXT_HEADER.encode("utf-8"):
            fail("sync did not normalize the windows header to utf-8 without CRLF")
        declaration = (root / CTP_REL / "linux" / "error.xml").read_text(encoding="utf-8").splitlines()[0]
        if declaration != '<?xml version="1.0" encoding="utf-8"?>':
            fail(f"sync did not rewrite the error.xml declaration: {declaration!r}")

        doc = (root / DOC_REL).read_text(encoding="utf-8")
        for probe in ("| 版本 | **9.9.9** |", "| 发布包内部版本 | `v9.9.9_20990101` |", f"`{sha256_file(package)}`"):
            if probe not in doc:
                fail(f"sync did not record {probe!r}")
        remaining = check(root)
        if remaining:
            fail(f"check() after sync not clean: {[v.render() for v in remaining]}")

        # 2) 第二次 sync：幂等（文件与记录都不变）
        before = _snapshot(root)
        with contextlib.redirect_stdout(io.StringIO()):
            sync(package, root)
        if _snapshot(root) != before:
            fail("sync is not idempotent: second run changed files")

        # 3) 包哈希记录不符时必须拒绝
        _write_doc_skeleton(root, "f" * 64, "0.0.0", "v0.0.0_19700101", package.name)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                sync(package, root)
        except VendorError:
            pass
        else:
            fail("sync accepted a package whose sha256 does not match the record")

    if not failures:
        print("selftest ok sync end-to-end (transform / 64-bit selection / record / idempotent / sha gate)")
    return failures


def selftest() -> int:
    failures = _check_selftest_cases() + _sync_selftest_case()
    if failures:
        print(f"selftest: {failures} failure(s)")
        return 1
    print("selftest: all known-bad forms detected and sync path verified")
    return 0


# ---- 入口 ------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="CTP 发布包入库规范化与 vendored 记录校验（记录真相：third_party/ctp/VENDORED.md）"
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--check", action="store_true", help="只读校验仓库文件与 VENDORED.md 一致")
    action.add_argument("--sync", action="store_true", help="从发布包 zip 重新生成文件并回写 VENDORED.md")
    action.add_argument("--selftest", action="store_true", help="注入已知违例自测校验器（不触碰工作区）")
    parser.add_argument("--zip", type=Path, help="发布包路径（仅 --sync 使用）")
    parser.add_argument("--root", help="仓库根（仅 --check / --sync 使用）")
    args = parser.parse_args(argv)

    if args.selftest:
        if args.zip is not None or args.root is not None:
            parser.error("--selftest takes neither --zip nor --root")
        return selftest()
    if args.sync:
        if args.zip is None:
            parser.error("--sync requires --zip")
    elif args.zip is not None:
        parser.error("--zip is only valid with --sync")

    try:
        root = repo_root(args.root)
        if args.check:
            violations = check(root)
            for violation in violations:
                print(violation.render())
            if violations:
                print(f"vendor-ctp: {len(violations)} violation(s)")
                return 1
            print(f"OK: {len(EXPECTED_FILES)} vendor files match {DOC_REL.as_posix()}")
            return 0

        assert args.zip is not None
        sync(args.zip.resolve(), root)
        violations = check(root)
        for violation in violations:
            print(violation.render())
        return 1 if violations else 0
    except VendorError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
