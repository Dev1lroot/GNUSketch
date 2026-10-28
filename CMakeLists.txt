cmake_minimum_required(VERSION 3.21)
project(GNUSketch VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(GNUSKETCH_BUILD_TESTS "Build kernel unit tests" ON)

# ---- geometry kernel (no Qt dependency) -------------------------------------
add_library(gs_core STATIC
  src/core/Math.h
  src/core/Material.h
  src/core/Triangulate.h src/core/Triangulate.cpp
  src/core/Mesh.h src/core/Mesh.cpp
  src/core/RoundCorners.h src/core/RoundCorners.cpp
  src/core/FollowMe.h src/core/FollowMe.cpp
  src/core/ImageTrace.h src/core/ImageTrace.cpp
  src/core/Units.h src/core/Units.cpp
  src/core/Document.h src/core/Document.cpp
  src/core/Components.h src/core/Components.cpp
  src/core/Terrain.h src/core/Terrain.cpp
  src/core/Solids.h src/core/Solids.cpp
)
target_include_directories(gs_core PUBLIC src)
target_compile_options(gs_core PRIVATE -Wall -Wextra)

# ---- application --------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Svg)
set(CMAKE_AUTOMOC ON)

add_library(gs_io STATIC
  src/io/NativeFormat.h src/io/NativeFormat.cpp
  src/io/ObjIO.h src/io/ObjIO.cpp
  src/io/GltfExport.h src/io/GltfExport.cpp
  src/io/StlExport.h src/io/StlExport.cpp
  src/io/ImageUtil.h src/io/ImageUtil.cpp
)
target_link_libraries(gs_io PUBLIC gs_core Qt6::Core Qt6::Gui)
target_compile_options(gs_io PRIVATE -Wall -Wextra)

add_executable(gnusketch
  src/main.cpp
  src/render/Camera.h src/render/Camera.cpp
  src/render/Renderer.h src/render/Renderer.cpp
  src/app/Inference.h src/app/Inference.cpp
  src/app/Tool.h src/app/Tool.cpp
  src/app/DrawTools.h src/app/DrawTools.cpp
  src/app/ModifyTools.h src/app/ModifyTools.cpp
  src/app/TerrainTools.h src/app/TerrainTools.cpp
  src/app/SolidTools.h src/app/SolidTools.cpp
  src/app/CameraTools.h src/app/CameraTools.cpp
  src/app/Viewport.h src/app/Viewport.cpp
  src/app/Icons.h src/app/Icons.cpp
  src/app/Panels.h src/app/Panels.cpp
  src/app/MainWindow.h src/app/MainWindow.cpp
  src/app/SelfTest.h src/app/SelfTest.cpp
)
target_link_libraries(gnusketch PRIVATE gs_io Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Svg)
target_compile_options(gnusketch PRIVATE -Wall -Wextra)

install(TARGETS gnusketch RUNTIME DESTINATION bin)
install(FILES packaging/gnusketch.desktop DESTINATION share/applications)

if(GNUSKETCH_BUILD_TESTS)
  enable_testing()
  add_executable(gs_tests tests/test_core.cpp)
  target_link_libraries(gs_tests PRIVATE gs_core)
  add_test(NAME core COMMAND gs_tests)

  add_executable(gs_io_tests tests/test_io.cpp)
  target_link_libraries(gs_io_tests PRIVATE gs_io)
  add_test(NAME io COMMAND gs_io_tests)
endif()
