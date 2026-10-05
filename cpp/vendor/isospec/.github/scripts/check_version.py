import subprocess
from pathlib import Path

# The tree has exactly one version number, in the VERSION file at its root:
# CMakeLists.txt reads it with file(READ), and pyproject.toml pulls it in as
# dynamic metadata (its [[tool.dynamic-metadata]] block). Read it here too --
# parsing pyproject.toml for a literal `version =` no longer finds anything,
# since there is no such key any more.
REPO_ROOT = Path(__file__).resolve().parents[2]


def git_version():
    version = subprocess.check_output(
        ["git", "describe", "--tags", "--abbrev=0"],
        stderr=subprocess.STDOUT
    ).strip().decode('utf-8')
    return version

def declared_version():
    version_file = REPO_ROOT / "VERSION"
    version = version_file.read_text(encoding="utf-8").strip()
    if not version:
        raise RuntimeError(f"{version_file} is empty")
    return version

if __name__ == "__main__":
    assert git_version() == "v"+declared_version(), \
        f"Version mismatch: git version '{git_version()}' != VERSION file '{declared_version()}'"
    print("Version check passed:", git_version())
