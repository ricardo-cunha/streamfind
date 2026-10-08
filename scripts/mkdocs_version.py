"""Inject the canonical streamfind version into Markdown pages."""

from pathlib import Path


_TOKEN = "{{ streamfind_version }}"


def on_page_markdown(markdown, page, config, files):
    """Replace the docs version token using the repository VERSION file."""
    root = Path(config.config_file_path).resolve().parent
    version_file = root / "VERSION"
    version = version_file.read_text(encoding="utf-8").strip()
    if not version:
        raise RuntimeError("VERSION file is empty")
    return markdown.replace(_TOKEN, version)
