cmake_minimum_required(VERSION 3.16)
project(BackgroundImageScenePlugin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Find Qt6
find_package(Qt6 COMPONENTS Widgets OpenGL REQUIRED)

# Source and header paths
# Set AVOGADRO_SRC_DIR to the avogadrolibs source tree for headers
# Set EIGEN_SRC_DIR to the Eigen source tree
# These can be overridden via -DAVOGADRO_SRC_DIR=... -DEIGEN_SRC_DIR=...
if(NOT AVOGADRO_SRC_DIR)
  set(AVOGADRO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../avogadrolibs")
endif()
if(NOT EIGEN_SRC_DIR)
  set(EIGEN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../eigen")
endif()

# Source files
set(SOURCES
  backgroundimageengine.cpp
)

# Build as a shared module (plugin)
add_library(BackgroundImageScenePlugin MODULE ${SOURCES})

# Include headers from the avogadrolibs source tree + stub exports + Eigen
target_include_directories(BackgroundImageScenePlugin
  PRIVATE
    ${AVOGADRO_SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated_exports
    ${EIGEN_SRC_DIR}
)

# Link against Qt6 and installed Avogadro libraries
# Find versioned .so.N (dev symlinks may be absent)
find_library(AVOGADRO_QTGUI_LIB
  NAMES AvogadroQtGui AvogadroQtGui.so.1
  PATHS /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu
)
find_library(AVOGADRO_RENDERING_LIB
  NAMES AvogadroRendering AvogadroRendering.so.1
  PATHS /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu
)
if(NOT AVOGADRO_QTGUI_LIB OR NOT AVOGADRO_RENDERING_LIB)
  message(FATAL_ERROR
    "Avogadro libraries not found. Install avogadrolibs-devel or pass\n"
    "  -DAVOGADRO_QTGUI_LIB=/path/to/libAvogadroQtGui.so.1\n"
    "  -DAVOGADRO_RENDERING_LIB=/path/to/libAvogadroRendering.so.1"
  )
endif()

target_link_libraries(BackgroundImageScenePlugin
  PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    ${AVOGADRO_QTGUI_LIB}
    ${AVOGADRO_RENDERING_LIB}
)

# No "lib" prefix for Qt plugins
set_target_properties(BackgroundImageScenePlugin PROPERTIES
  PREFIX ""
)

# Install target
include(GNUInstallDirs)
install(TARGETS BackgroundImageScenePlugin
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/avogadro2/plugins
)
