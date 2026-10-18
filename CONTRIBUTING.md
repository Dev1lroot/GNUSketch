# Contributing to GNUSketch

Thanks for helping! A few ground rules keep the project healthy.

## Clean-room policy

GNUSketch re-implements the *behaviour* and general look of SketchUp-style
modelling. It must never contain anything taken from proprietary products:

* no code, decompiled or otherwise derived from closed-source software;
* no icons, images, textures, fonts or UI artwork copied from them (all icons
  are original SVGs in `src/app/Icons.cpp`);
* no support for proprietary file formats that requires their SDKs or reverse
  engineering. Interchange goes through open formats (OBJ, glTF, STL).

Public screenshots and documentation may be *looked at* for behaviour and
layout, but don't commit them to the repository.

## Building and testing

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
tools/headless-selftest.sh build   # scripted UI run on Xvfb, no window on your desktop
```

New geometry behaviour should come with a test in `tests/test_core.cpp`.

## Code style

* C++20, 4-space indentation, ~120 columns, `camelCase` methods, `trailing_` members.
* The geometry kernel in `src/core` has no Qt dependency. Keep it that way.
* Every source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`.

## License

By contributing you agree that your contributions are licensed under the
GNU General Public License v3.0 or later.
