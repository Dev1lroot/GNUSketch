cmake_minimum_required(VERSION 3.21)
project(GNUSketch VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(GNUSKETCH_BUILD_TESTS "Build kernel unit tests" ON)

include(GNUInstallDirs)
# The kernel and the file code are shared libraries: plugins (plugins/, see
# src/plugin/FormatPlugin.h) use them too, and there must be one of each in the
# program (one mesh revision counter, one texture revision counter).
set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
set(GNUSKETCH_PLUGIN_BUILD_DIR ${CMAKE_BINARY_DIR}/plugins)
set(GNUSKETCH_PLUGIN_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/gnusketch/plugins)

# ---- geometry kernel (no Qt dependency) -------------------------------------
add_library(gs_core SHARED
  src/core/Math.h
  src/core/Material.h src/core/Material.cpp
  src/core/Triangulate.h src/core/Triangulate.cpp
  src/core/Mesh.h src/core/Mesh.cpp
  src/core/RoundCorners.h src/core/RoundCorners.cpp
  src/core/FollowMe.h src/core/FollowMe.cpp
  src/core/ImageTrace.h src/core/ImageTrace.cpp
  src/core/Units.h src/core/Units.cpp
  src/core/Document.h src/core/Document.cpp
  src/core/Components.h src/core/Components.cpp
  src/core/Animation.h src/core/Animation.cpp
  src/core/Timeline.h src/core/Timeline.cpp
  src/core/Physics.h src/core/Physics.cpp
  src/core/Terrain.h src/core/Terrain.cpp
  src/core/Solids.h src/core/Solids.cpp
  src/core/Effects.h src/core/Effects.cpp
  src/core/Walker.h src/core/Walker.cpp
  src/core/TextureMapping.h src/core/TextureMapping.cpp
  src/core/Primitives.h src/core/Primitives.cpp
)
target_include_directories(gs_core PUBLIC src)
target_compile_options(gs_core PRIVATE -Wall -Wextra)

# ---- application --------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Svg Qml)
set(CMAKE_AUTOMOC ON)

add_library(gs_io SHARED
  src/io/NativeFormat.h src/io/NativeFormat.cpp
  src/io/ObjIO.h src/io/ObjIO.cpp
  src/io/GltfExport.h src/io/GltfExport.cpp
  src/io/StlExport.h src/io/StlExport.cpp
  src/io/PrintExport.h src/io/PrintExport.cpp
  src/io/PrintImport.h src/io/PrintImport.cpp
  src/io/ImageUtil.h src/io/ImageUtil.cpp
  src/io/AssetLibrary.h src/io/AssetLibrary.cpp
  src/io/ArchMaterials.h src/io/ArchMaterials.cpp
  src/io/EntityScript.h src/io/EntityScript.cpp
)
find_package(ZLIB REQUIRED)
target_link_libraries(gs_io PUBLIC gs_core Qt6::Core Qt6::Gui Qt6::Qml PRIVATE ZLIB::ZLIB)
target_compile_options(gs_io PRIVATE -Wall -Wextra)

add_executable(gnusketch
  src/main.cpp
  src/render/Camera.h src/render/Camera.cpp
  src/render/Renderer.h src/render/Renderer.cpp
  src/render/RayScene.h src/render/RayScene.cpp
  src/render/OffscreenRenderer.h src/render/OffscreenRenderer.cpp
  src/app/Inference.h src/app/Inference.cpp
  src/app/Tool.h src/app/Tool.cpp
  src/app/Theme.h src/app/Theme.cpp
  src/app/Properties.h src/app/Properties.cpp
  src/app/Animator.h src/app/Animator.cpp
  src/app/Screenshots.cpp
  src/app/DrawTools.h src/app/DrawTools.cpp
  src/app/ModifyTools.h src/app/ModifyTools.cpp
  src/app/TerrainTools.h src/app/TerrainTools.cpp
  src/app/SolidTools.h src/app/SolidTools.cpp
  src/app/GameMode.h src/app/GameMode.cpp
  src/app/BendTool.h src/app/BendTool.cpp
  src/app/TextureTool.h src/app/TextureTool.cpp
  src/app/EffectsUi.h src/app/EffectsUi.cpp
  src/app/SceneTools.h src/app/SceneTools.cpp
  src/app/Hierarchy.h src/app/Hierarchy.cpp
  src/app/EntityProperties.h src/app/EntityProperties.cpp
  src/app/MaterialEditor.h src/app/MaterialEditor.cpp
  src/app/Browsers.h src/app/Browsers.cpp
  src/app/CameraTools.h src/app/CameraTools.cpp
  src/app/Viewport.h src/app/Viewport.cpp
  src/app/Icons.h src/app/Icons.cpp
  src/app/Panels.h src/app/Panels.cpp
  src/app/MainWindow.h src/app/MainWindow.cpp
  src/app/Plugins.h src/app/Plugins.cpp
  src/plugin/FormatPlugin.h
  src/app/SelfTest.h src/app/SelfTest.cpp
)
target_link_libraries(gnusketch PRIVATE gs_io Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Svg)
target_compile_definitions(gnusketch PRIVATE GNUSKETCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}" GNUSKETCH_VERSION="${PROJECT_VERSION}")
target_compile_options(gnusketch PRIVATE -Wall -Wextra)

install(TARGETS gnusketch RUNTIME DESTINATION bin)
install(TARGETS gs_core gs_io LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/plugin/FormatPlugin.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gnusketch/plugin)

# ---- plugins ----------------------------------------------------------------------
add_subdirectory(plugins/obm)
install(FILES packaging/gnusketch.desktop DESTINATION share/applications)
install(FILES packaging/gnusketch.svg DESTINATION share/icons/hicolor/scalable/apps)

if(GNUSKETCH_BUILD_TESTS)
  enable_testing()
  add_executable(gs_tests tests/test_core.cpp)
  target_link_libraries(gs_tests PRIVATE gs_core)
  add_test(NAME core COMMAND gs_tests)

  add_executable(gs_io_tests tests/test_io.cpp)
  target_link_libraries(gs_io_tests PRIVATE gs_io)
  add_test(NAME io COMMAND gs_io_tests)

  add_executable(gs_obm_tests tests/test_obm.cpp)
  target_link_libraries(gs_obm_tests PRIVATE gs_obm_format gs_io)
  add_test(NAME obm COMMAND gs_obm_tests)
endif()
