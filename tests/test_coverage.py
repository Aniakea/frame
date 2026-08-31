import json
from pathlib import Path

import pytest
from frame_tools.coverage import branch_percentage


def test_reads_llvm_branch_percentage(tmp_path: Path) -> None:
    report = tmp_path / "coverage.json"
    report.write_text(
        json.dumps({"data": [{"totals": {"branches": {"percent": 91.25}}}]}),
        encoding="utf-8",
    )

    assert branch_percentage(report) == 91.25


def test_rejects_invalid_llvm_export(tmp_path: Path) -> None:
    report = tmp_path / "coverage.json"
    report.write_text("{}", encoding="utf-8")

    with pytest.raises(ValueError, match="invalid llvm-cov export"):
        branch_percentage(report)
