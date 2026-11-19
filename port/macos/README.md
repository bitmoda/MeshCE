# OpenCE on Apple Silicon

This is upstream OpenCE with a Mac host. The game is the same source the Windows and Linux ports compile: the same network protocol, the same ports, and the same menus. The Mac app is the window, input, the local network prompt, and the disk image. Drawing goes through the guest's OpenGL ES renderer, and ANGLE presents that with Metal.

The app is `OpenCE.app`. Its bundle id is `local.opence.apple`. Saves go in `~/Library/Application Support/OpenCE`. That folder is separate from MeshCE.

Build it from the repository root:

```
python3 tools/macos_setup.py
python3 tools/macos_build.py --no-data-path
```

The result is `build/macos/OpenCE.app`. The first launch asks for the game data. Updates stay off unless a build number is passed. A numbered build looks at `bitmoda/MeshCE` for `opence-build-N` and `opence-testing-N`, and for `opence-macos-arm64.dmg`.
