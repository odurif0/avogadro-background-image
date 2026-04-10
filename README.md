# Background Image Scene Plugin for Avogadro 2

Standalone plugin that displays a user-loaded image as the viewport background in [Avogadro 2](https://avogadro.cc/), allowing molecule construction on top of the image (like a puzzle guide).

## Features

- Load any image (PNG, JPG, BMP, TIFF) as viewport background
- Molecule renders on top with correct transparency
- Simple UI: load/clear buttons in Display Type Settings

## Requirements

- Avogadro 2 (tested with 1.100.0 / avogadrolibs 1.100.0)
- Qt 6
- CMake 3.16+
- avogadrolibs source headers and Eigen headers (for compilation)

## Build

```bash
mkdir build && cd build
cmake .. \
  -DAVOGADRO_SRC_DIR=/path/to/avogadrolibs \
  -DEIGEN_SRC_DIR=/path/to/eigen \
  -DAVOGADRO_QTGUI_LIB=/usr/lib64/libAvogadroQtGui.so.1 \
  -DAVOGADRO_RENDERING_LIB=/usr/lib64/libAvogadroRendering.so.1
make
```

## Install

```bash
sudo cp BackgroundImageScenePlugin.so /usr/lib64/avogadro2/plugins/
```

## Usage

1. Launch Avogadro 2
2. Open **View → Display Type Settings**
3. Enable **Background Image**
4. Click **Load Image...** and select an image file
5. Build your molecule on top of the image

## How it works

The plugin uses Avogadro's `Overlay2DPass` (last render pass):

1. Copies the current framebuffer (with molecule) to a texture
2. Draws the user's background image fullscreen
3. Recomposites the molecule on top via a GLSL shader that makes black (empty) areas transparent

## License

MIT

---

*Developed with assistance from [GLM-5.1](https://chatglm.ai).*
