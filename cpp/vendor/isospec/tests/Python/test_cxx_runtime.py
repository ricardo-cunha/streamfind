"""The shipped library must not drag its C++ runtime into other people's processes.

macOS wheels are built with Homebrew GCC (Apple's libc++ has no
``<experimental/simd>``) and link libstdc++ statically, because delocate refuses
to bundle a dylib whose minimum macOS version is newer than the wheel's.  A
static runtime that keeps default visibility is a trap: Mach-O coalesces weak
definitions -- templates, inline functions, vtables -- across *all* loaded
images regardless of two-level namespace, so any process that also loads
something linked against Homebrew's ``libstdc++.6.dylib`` gets two copies of the
runtime spliced together and aborts inside ``dlopen``.  No dyld message, no
Python exception: the interpreter simply dies.

``skbuild/CMakeLists.txt`` therefore links the archive with ``-load_hidden``.
These tests fail -- deliberately not skip -- if that ever comes undone.
"""

import platform
import re
import shutil
import subprocess

import pytest

from IsoSpecPy.isoFFI import isoFFI  # the loader singleton, not the module


# Exported symbols that would mean part of a C++ standard library is visible in
# our shared object.  Mach-O prefixes symbol names with '_', so a mangled C++
# name starts with "__Z".
_RUNTIME_SYMBOL_RE = re.compile(
    r"^__Z("
    r"N?K?(St|Ss)"             # std::, std::string
    r"|TV?I?S[st]"             # vtables / typeinfo for std::
    r"|N?K?9__gnu_cxx"         # libstdc++ internals
    r"|nwm?|nam?|dlPv|daPv"    # operator new / delete
    r")")


def _exported_symbols(libpath):
    """Names of the symbols our shared library *defines* and exports."""
    nm = shutil.which("nm")
    if nm is None:
        pytest.skip("nm not available, cannot inspect the library's symbol table")
    out = subprocess.run([nm, "-gU", str(libpath)], capture_output=True, text=True)
    if out.returncode != 0:
        pytest.skip(f"nm failed on {libpath}: {out.stderr.strip()}")
    return [line.split()[-1] for line in out.stdout.splitlines() if line.split()]


@pytest.mark.skipif(platform.system() != "Darwin",
                    reason="only the macOS build carries its own C++ runtime; "
                           "elsewhere everyone shares one libstdc++.so and "
                           "exported template instantiations are normal")
def test_no_cxx_runtime_symbols_exported():
    """We export our own API, never a copy of the C++ standard library."""
    libpath = isoFFI.libpath
    leaked = sorted(s for s in _exported_symbols(libpath)
                    if _RUNTIME_SYMBOL_RE.match(s))
    assert not leaked, (
        f"{libpath} exports {len(leaked)} C++ standard library symbols, e.g. "
        f"{leaked[:5]}. A statically linked runtime must be hidden "
        "(-Wl,-load_hidden on macOS), or it will collide with every other "
        "libstdc++ in the process and abort it inside dlopen.")


def test_coexists_with_another_libstdcxx(old_isospec_probe):
    """A second libstdc++ in the process must not take the interpreter down.

    OldIsoSpecPy is the canary: the wheel test venv builds it with the same
    Homebrew GCC, against the *shared* libstdc++, which is exactly the
    configuration that used to abort.
    """
    if old_isospec_probe.status in ("missing", "unusable"):
        pytest.skip(old_isospec_probe.detail)
    assert old_isospec_probe.status == "ok", old_isospec_probe.detail
