"""Fixtures shared across the Python test suite.

The only thing here is the out-of-process OldIsoSpecPy probe.  It lives in a
fixture rather than in ``test_IsoSpecPy.py`` because two test modules need it:
``test_cxx_runtime.py`` asserts on its result, and ``test_IsoSpecPy.py`` uses it
to decide whether importing OldIsoSpecPy in *this* interpreter is safe.
"""

import subprocess
import sys
from collections import namedtuple

import pytest


# Exit codes of the probe below.  Anything else -- including death by signal --
# means the two libraries cannot coexist, which is a failure, not a skip.
_OK = 0
_MISSING = 3
_UNUSABLE = 4

_PROBE = f"""
import importlib.util, sys
if importlib.util.find_spec("OldIsoSpecPy") is None:
    sys.exit({_MISSING})
import IsoSpecPy               # our C++ runtime first, as the test suite does
try:
    import OldIsoSpecPy
except Exception:
    sys.exit({_UNUSABLE})
sys.exit({_OK})
"""

OldIsoSpecProbe = namedtuple("OldIsoSpecProbe", "status detail")


@pytest.fixture(scope="session")
def old_isospec_probe():
    """Can IsoSpecPy and OldIsoSpecPy live in one interpreter?

    Answered in a subprocess, because the interesting failure mode is not an
    exception: two copies of libstdc++ in one process abort inside ``dlopen``,
    killing the interpreter outright (see ``skbuild/CMakeLists.txt``).  Asking
    in-process would take the whole pytest session down with it.

    ``status`` is one of ``ok`` / ``missing`` / ``unusable`` / ``broken``.
    """
    try:
        proc = subprocess.run([sys.executable, "-c", _PROBE],
                              capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired as e:
        return OldIsoSpecProbe(
            "broken",
            "importing IsoSpecPy and OldIsoSpecPy in one interpreter hung "
            f"(no exit within {e.timeout}s)"
            f"\n--- stdout ---\n{e.stdout}\n--- stderr ---\n{e.stderr}")
    if proc.returncode == _OK:
        return OldIsoSpecProbe("ok", "")
    if proc.returncode == _MISSING:
        return OldIsoSpecProbe("missing", "OldIsoSpecPy is not installed")
    if proc.returncode == _UNUSABLE:
        # OldIsoSpecPy ships prebuilt DLLs for x86 Windows only, so on e.g.
        # win-arm64 it imports and then raises while loading its C++ part.
        # That is the package's own limitation, and a legitimate skip.
        return OldIsoSpecProbe(
            "unusable", "OldIsoSpecPy cannot load its C++ part on this platform")
    return OldIsoSpecProbe(
        "broken",
        "importing IsoSpecPy and OldIsoSpecPy in one interpreter failed with "
        f"exit code {proc.returncode}"
        + (" (killed by signal)" if proc.returncode < 0 else "")
        + f"\n--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}")
