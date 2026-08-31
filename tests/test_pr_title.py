from frame_tools.pr_title import validate_title


def test_accepts_conventional_title() -> None:
    assert validate_title("feat(runtime): add lifecycle gate") is None
    assert validate_title("fix!: reject stale handles") is None


def test_rejects_unstructured_title() -> None:
    assert validate_title("Add lifecycle gate") is not None
    assert validate_title("feature: add lifecycle gate") is not None
