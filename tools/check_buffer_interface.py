#!/usr/bin/env python3
"""Reject new external dependencies on the memline buffer interface.

By default this checks added lines relative to ``HEAD`` and untracked source
files.  Use ``--all`` to audit every source file; that mode reports the legacy
accesses that still need migration.  Memline/memfile implementation files and
the buffer façade/lifecycle code are source-file exceptions.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path
from typing import cast

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".inc", ".pro"}
IMPLEMENTATION_FILES = {
    "src/buffer.c",
    "src/memfile.c",
    "src/memline.c",
    "src/proto/memline.pro",
}
RULES = (
    ("direct b_ml access", re.compile(r"(?:->|\.)b_ml\b")),
    ("memline line pointer access", re.compile(r"(?:->|\.)ml_line_ptr\b")),
    (
        "implicit current-buffer text access",
        re.compile(
            r"\bml_(?:get|get_len|replace|replace_len|append|append_flags|delete|delete_flags)\s*\("
        ),
    ),
    ("BUFEMPTY macro", re.compile(r"\bBUFEMPTY\s*\(")),
)


def source_path(path: str) -> bool:
    return path.startswith("src/") and Path(path).suffix.lower() in SOURCE_SUFFIXES


def added_lines(repo_root: Path) -> list[tuple[str, int, str]]:
    result = subprocess.run(
        ["git", "diff", "HEAD", "--no-ext-diff", "--unified=0", "--no-color", "--"],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    )
    findings: list[tuple[str, int, str]] = []
    path = None
    line_number = 0
    for line in result.stdout.splitlines():
        if line.startswith("+++ b/"):
            path = line[6:]
            continue
        if not line.startswith("@@"):
            if path is not None and line.startswith("+") and not line.startswith("+++"):
                findings.append((path, line_number, line[1:]))
                line_number += 1
            continue
        match = re.search(r"\+([0-9]+)(?:,([0-9]+))?", line)
        if match:
            line_number = int(match.group(1))
    return findings


def untracked_lines(repo_root: Path) -> list[tuple[str, int, str]]:
    result = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "--", "src/"],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    )
    lines: list[tuple[str, int, str]] = []
    for relative in result.stdout.splitlines():
        if not source_path(relative):
            continue
        path = repo_root / relative
        for line_number, text in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            lines.append((relative, line_number, text))
    return lines


def all_lines(repo_root: Path) -> list[tuple[str, int, str]]:
    source_root = repo_root / "src"
    lines: list[tuple[str, int, str]] = []
    for path in sorted(source_root.rglob("*")):
        relative = path.relative_to(repo_root).as_posix()
        if not path.is_file() or not source_path(relative):
            continue
        for line_number, text in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            lines.append((relative, line_number, text))
    return lines


def check(repo_root: Path, audit_all: bool) -> list[str]:
    lines = (
        all_lines(repo_root)
        if audit_all
        else added_lines(repo_root) + untracked_lines(repo_root)
    )
    errors: list[str] = []
    for path, line_number, text in lines:
        stripped = text.lstrip()
        if (
            not source_path(path)
            or path in IMPLEMENTATION_FILES
            or stripped.startswith(("//", "/*", "*"))
        ):
            continue
        for name, pattern in RULES:
            if pattern.search(text):
                errors.append(f"{path}:{line_number}: [{name}] {text}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Reject new external buffer/memline interface accesses."
    )
    _ = parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="repository root (default: inferred from this script's location)",
    )
    _ = parser.add_argument(
        "--all",
        action="store_true",
        help="audit all source files instead of only added diff lines",
    )
    args = parser.parse_args()
    root_arg = cast(Path | None, args.root)
    audit_all = cast(bool, args.all)
    repo_root = (root_arg or Path(__file__).resolve().parents[1]).resolve()
    errors = check(repo_root, audit_all)
    mode = "all source files" if audit_all else "added source lines"
    if errors:
        print(f"Buffer interface check failed for {mode}:")
        print("\n".join(errors))
        return 1
    print(f"Buffer interface check passed for {mode}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
