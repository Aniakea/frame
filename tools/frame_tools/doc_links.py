"""Check local targets and Markdown heading fragments in repository documentation."""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from collections.abc import Iterable, Sequence
from pathlib import Path
from urllib.parse import unquote, urlsplit

LINK_PATTERN = re.compile(r"!?\[[^]]*]\(\s*(<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\)")
HEADING_PATTERN = re.compile(r"^#{1,6}\s+(.+?)\s*#*\s*$")
INLINE_CODE_PATTERN = re.compile(r"`+[^`]*`+")
IGNORED_DIRECTORIES = {
    ".build",
    ".cache",
    ".git",
    ".opencode",
    ".pytest_cache",
    ".venv",
    ".venv-idf",
    "build",
    "managed_components",
}


def _slug(text: str) -> str:
    text = re.sub(r"<[^>]*>", "", text)
    text = re.sub(r"!\[[^]]*]\([^)]*\)", "", text)
    text = re.sub(r"\[([^]]*)]\([^)]*\)", r"\1", text)
    text = re.sub(r"[`*_~]", "", text).strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return re.sub(r"\s", "-", text)


def markdown_anchors(path: Path) -> set[str]:
    anchors: set[str] = set()
    seen: Counter[str] = Counter()
    in_fence = False
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = HEADING_PATTERN.match(line)
        if match is None:
            continue
        base = _slug(match.group(1))
        duplicate = seen[base]
        seen[base] += 1
        anchors.add(base if duplicate == 0 else f"{base}-{duplicate}")
    return anchors


def _markdown_files(root: Path, inputs: Sequence[Path]) -> tuple[list[Path], list[str]]:
    files: list[Path] = []
    errors: list[str] = []
    sources = list(inputs) if inputs else [root]
    for source in sources:
        if source.is_file() and source.suffix.lower() == ".md":
            files.append(source)
        elif source.is_dir():
            for path in source.rglob("*.md"):
                try:
                    relative_parts = path.relative_to(root).parts
                except ValueError:
                    relative_parts = path.parts
                if not any(part in IGNORED_DIRECTORIES for part in relative_parts):
                    files.append(path)
        else:
            errors.append(f"{source}: documentation path does not exist or is not Markdown")
    return sorted(set(files), key=lambda path: str(path)), errors


def _links(path: Path) -> Iterable[tuple[int, str]]:
    in_fence = False
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        line = INLINE_CODE_PATTERN.sub("", line)
        for match in LINK_PATTERN.finditer(line):
            target = match.group(1)
            yield line_number, target[1:-1] if target.startswith("<") else target


def validate_doc_links(root: Path, paths: Sequence[Path] = ()) -> list[str]:
    root = root.resolve()
    markdown_files, errors = _markdown_files(root, paths)
    anchor_cache: dict[Path, set[str]] = {}

    for source in markdown_files:
        try:
            links = list(_links(source))
        except (OSError, UnicodeError) as error:
            errors.append(f"{source}: cannot read Markdown: {error}")
            continue

        for line_number, raw_target in links:
            parsed = urlsplit(raw_target)
            if parsed.scheme or parsed.netloc:
                continue
            relative_target = Path(unquote(parsed.path))
            if not parsed.path:
                target = source.resolve()
            elif parsed.path.startswith("/"):
                target = (root / str(relative_target).lstrip("/")).resolve(strict=False)
            else:
                target = (source.parent / relative_target).resolve(strict=False)

            if not target.is_relative_to(root):
                errors.append(
                    f"{source}:{line_number}: local link escapes repository: {raw_target}"
                )
                continue
            if not target.exists():
                errors.append(
                    f"{source}:{line_number}: local link target does not exist: {raw_target}"
                )
                continue
            if parsed.fragment and target.suffix.lower() == ".md":
                anchors = anchor_cache.setdefault(target, markdown_anchors(target))
                fragment = unquote(parsed.fragment)
                if fragment not in anchors:
                    errors.append(
                        f"{source}:{line_number}: Markdown anchor does not exist: {raw_target}"
                    )
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate local links in Markdown files")
    parser.add_argument("paths", nargs="*", type=Path, help="Markdown files or directories")
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="repository root")
    args = parser.parse_args(argv)

    errors = validate_doc_links(args.root, args.paths)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("documentation links are valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
