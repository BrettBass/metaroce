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

"""Validates filter entries against test definitions in the fetched suite.
Missing definitions fail configuration to prevent silent loss of coverage.
For parameterized tests, only the fixture and test name are checked;
instantiation prefixes and parameter suffixes are not validated.
With --absent, entries must match no test definition; a skip that gains
a definition fails configuration to force promotion to the test filter.

usage: check_green_names.py [--absent] <suite_root> <entry>...
"""

import fnmatch
import re
import sys
from pathlib import Path

TEST = re.compile(
    r"\b(?:TEST_F|TEST_P|TYPED_TEST_P|TEST)\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)"
)


def defined_cases(root: str) -> set[str]:
    names = set()
    for cc in sorted(Path(root).glob("unit/*.cc")):
        for match in TEST.finditer(cc.read_text()):
            names.add(f"{match.group(1)}.{match.group(2)}")
    return names


def matches(entry: str, names: set[str]) -> bool:
    for part in entry.split("/"):
        if "." not in part:
            continue
        if "*" in part:
            return any(fnmatch.fnmatchcase(name, part) for name in names)
        return part in names
    return False


def main() -> int:
    args = sys.argv[1:]
    absent_only = args[:1] == ["--absent"]
    if absent_only:
        args = args[1:]
    root, entries = args[0], args[1:]
    names = defined_cases(root)
    if absent_only:
        present = [entry for entry in entries if matches(entry, names)]
        if present:
            print(
                "skip entries matching test definitions in the fetched suite:",
                file=sys.stderr,
            )
            for entry in present:
                print(f"  {entry}", file=sys.stderr)
            return 1
        print(f"{len(entries)} skip entries all absent from the fetched suite")
        return 0
    missing = [entry for entry in entries if not matches(entry, names)]
    if missing:
        print(
            "green entries matching no test definition in the fetched suite:",
            file=sys.stderr,
        )
        for entry in missing:
            print(f"  {entry}", file=sys.stderr)
        return 1
    print(
        f"{len(entries)} green entries all match test definitions in the fetched suite"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
