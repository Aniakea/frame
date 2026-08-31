"""Validate pull request titles without granting pull request code extra permissions."""

from __future__ import annotations

import argparse
import os
import re
import sys
from collections.abc import Sequence

TITLE_PATTERN = re.compile(
    r"^(?:build|chore|ci|docs|feat|fix|perf|refactor|revert|style|test)"
    r"(?:\([a-z0-9][a-z0-9._/-]*\))?!?: \S.*$"
)


def validate_title(title: str) -> str | None:
    """Return a diagnostic when a PR title does not follow the repository convention."""
    if title != title.strip() or "\n" in title or "\r" in title:
        return "PR title must be a single line without leading or trailing whitespace"
    if not TITLE_PATTERN.fullmatch(title):
        return (
            "PR title must use '<type>(optional-scope): summary'; allowed types are "
            "build, chore, ci, docs, feat, fix, perf, refactor, revert, style, and test"
        )
    return None


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate a Frame pull request title")
    parser.add_argument("--title", help="title to validate; defaults to the PR_TITLE environment")
    args = parser.parse_args(argv)

    title = args.title if args.title is not None else os.environ.get("PR_TITLE")
    if title is None:
        parser.error("--title or PR_TITLE is required")

    error = validate_title(title)
    if error is not None:
        print(error, file=sys.stderr)
        return 1
    print("PR title is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
