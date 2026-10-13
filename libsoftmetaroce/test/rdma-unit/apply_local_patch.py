#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Applies the patches/ series (or one patch file) to the fetched
rdma-unit-test tree.

usage: apply_local_patch.py <patch|patch_dir> <suite_root>
A directory applies its series file in order. Hunks must match their
expected positions and contents. Already-patched trees are a no-op.
"""

import re
import sys
from pathlib import Path

DIFF = re.compile(r"diff --git a/(\S+) b/\S+")
HUNK = re.compile(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def _body_lines(patch: str) -> list[str]:
    lines = patch.split("\n")
    if lines and not lines[-1]:
        # Trailing artifact of the final newline, not a hunk line.
        lines.pop()
    for line in lines:
        if line.startswith("\\ No newline"):
            sys.exit("unsupported hunk: missing trailing newline")
    return lines


def _new_hunk(files: dict[str, list[list]], path: str, m: re.Match) -> list:
    hunk: list = [int(m.group(1)), [], int(m.group(3)), []]
    hunk += [m.group(2), m.group(4)]
    files.setdefault(path, []).append(hunk)
    return hunk


def _collect(hunk: list, line: str) -> bool:
    if not line or line[0] == " ":
        body = line[1:] if line else ""
        hunk[1].append(body)
        hunk[3].append(body)
    elif line[0] == "-":
        hunk[1].append(line[1:])
    elif line[0] == "+":
        hunk[3].append(line[1:])
    else:
        return False
    return True


def _check_counts(path: str, hunk: list) -> None:
    old_start, old, _new_start, new, old_n, new_n = hunk
    want_old = int(old_n) if old_n is not None else 1
    want_new = int(new_n) if new_n is not None else 1
    if len(old) != want_old or len(new) != want_new:
        sys.exit(f"{path}: malformed hunk at {old_start}")


def parse(patch: str) -> dict[str, list[list]]:
    files: dict[str, list[list]] = {}
    path = None
    hunk = None
    for line in _body_lines(patch):
        if m := DIFF.match(line):
            path = m.group(1)
            hunk = None
        elif m := HUNK.match(line):
            if path is None:
                sys.exit("hunk before any file header")
            hunk = _new_hunk(files, path, m)
        elif line.startswith(("--- ", "+++ ")):
            hunk = None
        elif hunk is not None and not _collect(hunk, line):
            hunk = None
    for path, hunks in files.items():
        for h in hunks:
            _check_counts(path, h)
    return files


def apply_to(path: Path, hunks: list[list]) -> bool:
    text = path.read_text()
    trailing = text.endswith("\n")
    lines = text.split("\n")
    if trailing:
        lines.pop()
    if all(
        lines[new_start - 1 : new_start - 1 + len(new)] == new
        for _, _, new_start, new, _, _ in hunks
    ):
        return False
    shift = 0
    for old_start, old, _, new, _, _ in hunks:
        at = old_start - 1 + shift
        if lines[at : at + len(old)] != old:
            sys.exit(f"{path}: hunk at {old_start} does not match")
        lines[at : at + len(old)] = new
        shift += len(new) - len(old)
    path.write_text("\n".join(lines) + ("\n" if trailing else ""))
    return True


def apply_patch_text(text: str, root: Path) -> tuple[int, int]:
    files = parse(text)
    if not files:
        sys.exit("no hunks parsed")
    changed = [p for p, hs in files.items() if apply_to(root / p, hs)]
    return len(changed), len(files)


def main() -> int:
    if len(sys.argv) != 3:
        sys.exit("usage: apply_local_patch.py <patch|patch_dir> <suite_root>")
    src = Path(sys.argv[1])
    root = Path(sys.argv[2])
    if src.is_dir():
        series = src / "series"
        if not series.is_file():
            sys.exit(f"{src}: no series file")
        names = [
            s
            for line in series.read_text().split("\n")
            if (s := line.strip()) and not s.startswith("#")
        ]
        if not names:
            sys.exit(f"{series}: no patches listed")
        for name in names:
            try:
                patched, total = apply_patch_text((src / name).read_text(), root)
            except SystemExit as e:
                sys.exit(f"{name}: {e.code}")
            except FileNotFoundError:
                sys.exit(f"{name}: not found")
            if patched:
                print(f"{name}: patched {patched} of {total} files")
            else:
                print(f"{name}: already patched")
    else:
        patched, total = apply_patch_text(src.read_text(), root)
        if patched:
            print(f"patched {patched} of {total} files")
        else:
            print("already patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
