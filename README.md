# Background Image Scene Plugin for Avogadro

Standalone plugin that displays a user-loaded image as the viewport background in [Avogadro](https://avogadro.cc/), allowing molecule construction on top of the image (like a puzzle guide).

> **Branch `v1.x`** — targets Avogadro 1.x (avogadrolibs 1.100.0, `.so.1`). For Avogadro 2.x, use the `master` branch.

## Quick Install (pre-built binaries)

Download the latest `v1.x` release for your platform from the [Releases page](https://github.com/odurif0/avogadro-background-image/releases), then copy the plugin file to the Avogadro plugins directory.

### Linux

```bash
# System install (e.g. openSUSE, Fedora)
sudo cp BackgroundImageScenePlugin-linux-x86_64.so /usr/lib64/avogadro2/plugins/BackgroundImageScenePlugin.so

# Debian/Ubuntu
sudo cp BackgroundImageScenePlugin-linux-x86_64.so /usr/lib/x86_64-linux-gnu/avogadro2/plugins/BackgroundImageScenePlugin.so
```

### Windows

1. Download `BackgroundImageScenePlugin-windows-x86_64.dll`
2. Copy it to: `C:\Program Files\Avogadro2\lib\avogadro2\plugins\BackgroundImageScenePlugin.dll`
3. Restart Avogadro

### macOS

```bash
cp BackgroundImageScenePlugin-macos-arm64.so \
   "/Applications/Avogadro2.app/Contents/lib/avogadro2/plugins/BackgroundImageScenePlugin.so"
```

## Build from Source

### Requirements

- Avogadro installed (tested with 1.100.0)
- Qt 6
- CMake 3.16+
- [avogadrolibs](https://github.com/OpenChemistry/avogadrolibs) source (for headers)
- [Eigen](https://gitlab.com/libeigen/eigen) source (for headers)

### Compile

```bash
git clone --branch 1.100.0 https://github.com/OpenChemistry/avogadrolibs.git
git clone https://gitlab.com/libeigen/eigen.git
mkdir build && cd build
cmake .. \
  -DAVOGADRO_SRC_DIR=/path/to/avogadrolibs \
  -DEIGEN_SRC_DIR=/path/to/eigen \
  -DAVOGADRO_QTGUI_LIB=/usr/lib64/libAvogadroQtGui.so.1 \
  -DAVOGADRO_RENDERING_LIB=/usr/lib64/libAvogadroRendering.so.1
make
sudo make install
```

## Usage

1. Launch Avogadro
2. Open **View → Display Type Settings**
3. Enable **Background Image**
4. Click **Add Images...** to load one or more images
5. Select an image from the list to set it as background
6. Build your molecule on top of the image

## How it works

The plugin uses Avogadro's `Overlay2DPass` (last render pass):

1. Copies the current framebuffer (with molecule) to a texture
2. Draws the user's background image fullscreen
3. Recomposites the molecule on top via a GLSL shader that makes black (empty) areas transparent

## License

MIT

---

*Developed with assistance from GLM-5.1.*
