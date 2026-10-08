Vendored from https://codeberg.org/soundtouch/soundtouch (LGPL-2.1).

- Tag: 2.4.1
- Commit: 0047e0b1ecfceb041348579119bf79b73a322a3a ("Bump version to 2.4.1", 2026-03-29)
- License text: COPYING.TXT in this directory, copied verbatim from upstream.

Only the SoundTouch library itself is vendored, not the whole upstream
tree: `source/SoundStretch` (CLI tool), `source/SoundTouchDLL` (C wrapper,
unneeded since this bridge talks to the C++ API directly from a .cpp file),
`source/Android-lib`, `source/csharp-example`, and upstream's own build
files (CMakeLists.txt, autotools, .sln/.vcxproj) were left out — this
project builds the library from these sources with its own
plugins/obs-filters/cmake/soundtouch.cmake instead, mirroring how
rnnoise.cmake vendors RNNoise.

`BPMDetect.cpp`/`BPMDetect.h` are also left out: they implement tempo/BPM
detection, a feature AudioShift doesn't use, and nothing in SoundTouch.cpp
or the pitch-shifting path (TDStretch/RateTransposer) references them.

To update to a newer upstream version, re-run the same file list against
the new tag and update SOUNDTOUCH_VERSION in
plugins/obs-filters/cmake/soundtouch.cmake.
