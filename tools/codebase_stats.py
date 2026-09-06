from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
import sys
import tokenize
from typing import Iterable


TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".m", ".mm", ".kt", ".kts", ".java", ".py", ".rs", ".go",
    ".md", ".rst", ".txt", ".toml", ".yaml", ".yml", ".json",
    ".ini", ".cfg", ".qss", ".css", ".svg", ".sh", ".cmake",
    ".gradle", ".properties", ".xml",
}

CODE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".m", ".mm", ".kt", ".kts", ".java", ".py", ".rs", ".go",
}

IGNORED_NAMES = {
    "uv.lock",
}

CATEGORY_ORDER = (
    "Production code",
    "Tests / probes",
    "Specification / contracts",
    "Architecture / design",
    "Documentation",
    "Tooling / CI",
    "Configuration / build",
    "Styles / text assets",
    "Other text",
)

SPEC_KEYWORDS = {
    "spec", "specification", "contract", "format", "schema", "protocol",
    "invariant", "compatibility", "reference", "semantics", "requirements",
}
ARCH_KEYWORDS = {
    "architecture", "architectural", "design", "adr", "rationale",
    "decision", "concept", "model", "topology",
}


@dataclass(slots=True, frozen=True)
class FileStats:
    repo: str
    path: str
    category: str
    physical_lines: int
    nonblank_lines: int
    code_lines: int | None
    bytes_count: int


def _run_git(root: Path, *args: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *args],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    return completed.stdout.strip()


def _tracked_paths(root: Path) -> tuple[Path, ...]:
    raw = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout
    return tuple(root / item.decode("utf-8") for item in raw.split(b"\0") if item)


def _tokens(relative: str) -> set[str]:
    normalized = relative.lower().replace("-", "_").replace(".", "_").replace("/", "_")
    return {token for token in normalized.split("_") if token}


def _is_text_path(path: Path) -> bool:
    return path.name == "CMakeLists.txt" or path.suffix.casefold() in TEXT_SUFFIXES


def _is_test_or_probe(relative: str, suffix: str) -> bool:
    lower = relative.lower()
    name = Path(relative).name.lower()
    return (
        lower.startswith("tests/")
        or "/tests/" in lower
        or "/src/test/" in lower
        or lower.startswith("src/test/")
        or "probe" in name
        or name.startswith("test_")
        or name.endswith("_test" + suffix)
        or name.endswith("_tests" + suffix)
    )


def _category(relative: str) -> str:
    path = Path(relative)
    suffix = path.suffix.casefold()
    lower = relative.lower()
    tokens = _tokens(relative)

    if _is_test_or_probe(relative, suffix):
        return "Tests / probes"

    if lower.startswith(".github/") or lower.startswith("tools/") or lower.startswith("scripts/"):
        return "Tooling / CI"
    if lower.startswith("dev/"):
        return "Tooling / CI"

    is_doc = lower.startswith("docs/") or suffix in {".md", ".rst"}
    if is_doc:
        if tokens & SPEC_KEYWORDS:
            return "Specification / contracts"
        if tokens & ARCH_KEYWORDS:
            return "Architecture / design"
        return "Documentation"

    if suffix in CODE_SUFFIXES:
        return "Production code"

    if (
        path.name == "CMakeLists.txt"
        or suffix in {".cmake", ".toml", ".yaml", ".yml", ".json", ".ini", ".cfg", ".gradle", ".properties", ".xml"}
        or path.name.lower().startswith("gradle")
        or path.name.lower().startswith("settings.gradle")
        or path.name.lower().startswith("build.gradle")
    ):
        return "Configuration / build"

    if suffix in {".qss", ".css", ".svg"}:
        return "Styles / text assets"
    return "Other text"


def _python_code_lines(source: str) -> int:
    meaningful: set[int] = set()
    ignored = {
        tokenize.ENCODING, tokenize.ENDMARKER, tokenize.INDENT, tokenize.DEDENT,
        tokenize.NEWLINE, tokenize.NL, tokenize.COMMENT,
    }
    try:
        for token in tokenize.generate_tokens(iter(source.splitlines(keepends=True)).__next__):
            if token.type in ignored:
                continue
            meaningful.update(range(token.start[0], token.end[0] + 1))
    except (IndentationError, tokenize.TokenError):
        return sum(1 for line in source.splitlines() if line.strip() and not line.lstrip().startswith("#"))
    return len(meaningful)


def _c_like_code_lines(source: str) -> int:
    # Deliberately conservative dependency-free metric: count lines containing
    # non-comment tokens. It is meant for scale/trend comparisons, not parsing.
    in_block = False
    count = 0
    for raw_line in source.splitlines():
        line = raw_line
        index = 0
        has_code = False
        in_string: str | None = None
        escaped = False
        while index < len(line):
            ch = line[index]
            nxt = line[index + 1] if index + 1 < len(line) else ""
            if in_block:
                if ch == "*" and nxt == "/":
                    in_block = False
                    index += 2
                else:
                    index += 1
                continue
            if in_string is not None:
                has_code = True
                if escaped:
                    escaped = False
                elif ch == "\\":
                    escaped = True
                elif ch == in_string:
                    in_string = None
                index += 1
                continue
            if ch in {'"', "'"}:
                in_string = ch
                has_code = True
                index += 1
                continue
            if ch == "/" and nxt == "*":
                in_block = True
                index += 2
                continue
            if ch == "/" and nxt == "/":
                break
            if not ch.isspace():
                has_code = True
            index += 1
        if has_code:
            count += 1
    return count


def _code_lines(path: Path, source: str) -> int | None:
    suffix = path.suffix.casefold()
    if suffix not in CODE_SUFFIXES:
        return None
    if suffix == ".py":
        return _python_code_lines(source)
    return _c_like_code_lines(source)


def _file_stats(repo: str, root: Path, path: Path) -> FileStats | None:
    relative = path.relative_to(root).as_posix()
    if path.name in IGNORED_NAMES or not _is_text_path(path):
        return None
    try:
        raw = path.read_bytes()
        source = raw.decode("utf-8")
    except (OSError, UnicodeDecodeError):
        return None
    lines = source.splitlines()
    return FileStats(
        repo=repo,
        path=relative,
        category=_category(relative),
        physical_lines=len(lines),
        nonblank_lines=sum(1 for line in lines if line.strip()),
        code_lines=_code_lines(path, source),
        bytes_count=len(raw),
    )


def _totals(files: Iterable[FileStats]) -> dict[str, int]:
    items = tuple(files)
    return {
        "files": len(items),
        "physical_lines": sum(item.physical_lines for item in items),
        "nonblank_lines": sum(item.nonblank_lines for item in items),
        "code_lines": sum(item.code_lines or 0 for item in items),
        "bytes": sum(item.bytes_count for item in items),
    }


def _fmt(value: int) -> str:
    return f"{value:,}".replace(",", " ")


def _parse_repo(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("repo must be LABEL=PATH")
    label, path = value.split("=", 1)
    root = Path(path).resolve()
    if not label or not root.exists():
        raise argparse.ArgumentTypeError(f"invalid repo specification: {value}")
    return label, root


def _print_table(repos: list[tuple[str, Path]], stats: tuple[FileStats, ...], top: int) -> None:
    print("AERIS codebase statistics")
    print("=" * 96)
    for label, root in repos:
        branch = _run_git(root, "branch", "--show-current") or "(detached)"
        commit = _run_git(root, "rev-parse", "--short=12", "HEAD")
        print(f"{label:<10} {branch:<34} {commit}")
    print()

    grouped: dict[tuple[str, str], list[FileStats]] = defaultdict(list)
    for item in stats:
        grouped[(item.repo, item.category)].append(item)

    print(f"{'Repo':<10} {'Category':<28} {'Files':>7} {'Physical':>11} {'Nonblank':>11} {'Code-ish':>11}")
    print("-" * 96)
    for label, _ in repos:
        for category in CATEGORY_ORDER:
            items = grouped.get((label, category), [])
            if not items:
                continue
            total = _totals(items)
            has_code = any(item.code_lines is not None for item in items)
            code = _fmt(total["code_lines"]) if has_code else "—"
            print(
                f"{label:<10} {category:<28} {_fmt(total['files']):>7} "
                f"{_fmt(total['physical_lines']):>11} {_fmt(total['nonblank_lines']):>11} {code:>11}"
            )
        total = _totals(item for item in stats if item.repo == label)
        print(
            f"{label:<10} {'TRACKED TEXT TOTAL':<28} {_fmt(total['files']):>7} "
            f"{_fmt(total['physical_lines']):>11} {_fmt(total['nonblank_lines']):>11} {_fmt(total['code_lines']):>11}"
        )
        print("-" * 96)

    overall = _totals(stats)
    print(
        f"{'ALL':<10} {'TRACKED TEXT TOTAL':<28} {_fmt(overall['files']):>7} "
        f"{_fmt(overall['physical_lines']):>11} {_fmt(overall['nonblank_lines']):>11} {_fmt(overall['code_lines']):>11}"
    )

    category_totals: dict[str, dict[str, int]] = {}
    for category in CATEGORY_ORDER:
        total = _totals(item for item in stats if item.category == category)
        if total["files"]:
            category_totals[category] = total
    print("\nCross-repository blocks")
    print("-" * 96)
    for category, total in category_totals.items():
        print(
            f"{category:<28} {_fmt(total['files']):>7} files  "
            f"{_fmt(total['physical_lines']):>11} physical  {_fmt(total['nonblank_lines']):>11} nonblank"
        )

    largest = sorted(stats, key=lambda item: item.physical_lines, reverse=True)[:max(0, top)]
    if largest:
        print(f"\nLargest {len(largest)} tracked text files")
        print("-" * 96)
        for item in largest:
            print(f"{_fmt(item.physical_lines):>8}  {item.repo}:{item.path}  [{item.category}]")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Cross-repository AERIS line statistics, adapted from Persona Training Lab codebase_stats.py.",
    )
    parser.add_argument("--repo", action="append", required=True, type=_parse_repo, metavar="LABEL=PATH")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()

    repos: list[tuple[str, Path]] = args.repo
    stats: list[FileStats] = []
    repo_meta: dict[str, dict[str, str]] = {}
    for label, root in repos:
        repo_meta[label] = {
            "branch": _run_git(root, "branch", "--show-current") or "(detached)",
            "commit": _run_git(root, "rev-parse", "HEAD"),
        }
        for path in _tracked_paths(root):
            item = _file_stats(label, root, path)
            if item is not None:
                stats.append(item)

    frozen = tuple(stats)
    _print_table(repos, frozen, args.top)

    if args.json_out:
        payload = {
            "repositories": repo_meta,
            "totals": _totals(frozen),
            "categories": {
                category: _totals(item for item in frozen if item.category == category)
                for category in CATEGORY_ORDER
            },
            "per_repository": {
                label: {
                    "totals": _totals(item for item in frozen if item.repo == label),
                    "categories": {
                        category: _totals(
                            item for item in frozen
                            if item.repo == label and item.category == category
                        )
                        for category in CATEGORY_ORDER
                    },
                }
                for label, _ in repos
            },
            "files": [item.__dict__ if hasattr(item, "__dict__") else {
                "repo": item.repo,
                "path": item.path,
                "category": item.category,
                "physical_lines": item.physical_lines,
                "nonblank_lines": item.nonblank_lines,
                "code_lines": item.code_lines,
                "bytes": item.bytes_count,
            } for item in frozen],
        }
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
