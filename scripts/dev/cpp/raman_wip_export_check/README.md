# WITec WIP/export comparison

This development-only check compares every spectrum in supplied WITec text
exports against every graph emitted by the WIP reader. It uses the native
reader sources directly and is deliberately outside CTest because it requires
the external example corpus.

Build from the repository root with the supported Windows UCRT64 compiler:

```powershell
cmake -S scripts/dev/cpp/raman_wip_export_check `
  -B tmp/build/raman_wip_export_check -G Ninja `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build tmp/build/raman_wip_export_check
```

Run it with explicit paths to the WIP project and candidate exports:

```powershell
tmp/build/raman_wip_export_check/raman_wip_export_check.exe `
  'E:/example_files/raman/wip_format/REM-Raman_Beispieldateien/Kazamer-251008-Referenz.wip' `
  'E:/example_files/raman/wip_format/REM-Raman_Beispieldateien/Spektren-Referenz.txt'
```

The sample's reference export is expected to map its columns to WIP graph
indices 7, 10, 13, and 16. Maximum absolute tolerances are 0.006 cm⁻¹ for the
axis and 0.001 CCD counts for signal, chosen to accommodate the observed text
rounding. These checks establish only the supplied file/export pairing; they
do not imply support for other WITec versions or prove that similarly named
Mitte/Rand exports belong to this project. Pass those files separately as
candidate exports; a nonzero exit means the current WIP records do not match
them within the stated tolerances.
