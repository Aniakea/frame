from pathlib import Path

from frame_tools.sbom import generate_sbom, locked_components


def test_repository_sbom_has_locked_idf_and_lvgl() -> None:
    components = locked_components(Path("."))
    references = {str(component["bom-ref"]) for component in components}

    assert "pkg:github/espressif/esp-idf@6.0.2" in references
    assert "pkg:espressif/lvgl/lvgl@9.4.0" in references
    assert "pkg:github/google/googletest@52eb8108c5bdec04579160ae17225d66034bd723" in references


def test_sbom_generation_is_deterministic() -> None:
    assert generate_sbom(Path(".")) == generate_sbom(Path("."))
