from pathlib import Path

from frame_tools.doc_links import validate_doc_links


def test_existing_file_and_anchor_pass(tmp_path: Path) -> None:
    (tmp_path / "target.md").write_text("# Target Heading\n", encoding="utf-8")
    (tmp_path / "source.md").write_text("[target](target.md#target-heading)\n", encoding="utf-8")

    assert validate_doc_links(tmp_path) == []


def test_missing_target_is_reported(tmp_path: Path) -> None:
    (tmp_path / "source.md").write_text("[missing](missing.md)\n", encoding="utf-8")

    errors = validate_doc_links(tmp_path)

    assert errors
    assert "does not exist" in errors[0]


def test_ignored_virtual_environment_is_not_scanned(tmp_path: Path) -> None:
    ignored = tmp_path / ".venv-idf"
    ignored.mkdir()
    (ignored / "README.md").write_text("[missing](missing.md)\n", encoding="utf-8")

    assert validate_doc_links(tmp_path) == []


def test_ignored_opencode_state_is_not_scanned(tmp_path: Path) -> None:
    ignored = tmp_path / ".opencode" / "node_modules" / "example"
    ignored.mkdir(parents=True)
    (ignored / "README.md").write_text("[missing](missing.md)\n", encoding="utf-8")

    assert validate_doc_links(tmp_path) == []
