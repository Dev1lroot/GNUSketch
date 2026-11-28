cmake_minimum_required(VERSION 3.21)
project(Forma VERSION 0.3.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(FORMA_BUILD_TESTS "Build kernel unit tests" ON)

include(GNUInstallDirs)
# The kernel and the file code are shared libraries: plugins (plugins/, see
# src/plugin/FormatPlugin.h) use them too, and there must be one of each in the
# program (one mesh revision counter, one texture revision counter).
set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
set(FORMA_PLUGIN_BUILD_DIR ${CMAKE_BINARY_DIR}/plugins)
set(FORMA_PLUGIN_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/forma/plugins)

# ---- geometry kernel (no Qt dependency) -------------------------------------
add_library(forma_core SHARED
  src/core/Math.h
  src/core/Material.h src/core/Material.cpp
  src/core/Triangulate.h src/core/Triangulate.cpp
  src/core/Mesh.h src/core/Mesh.cpp
  src/core/RoundCorners.h src/core/RoundCorners.cpp
  src/core/GearNotch.h src/core/GearNotch.cpp
  src/core/Slicer.h src/core/Slicer.cpp
  src/core/ScrewThread.h src/core/ScrewThread.cpp
  src/core/MechParts.h src/core/MechParts.cpp
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
target_include_directories(forma_core PUBLIC src)
target_compile_options(forma_core PRIVATE -Wall -Wextra)

# ---- application --------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Svg Qml)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_library(forma_io SHARED
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
target_link_libraries(forma_io PUBLIC forma_core Qt6::Core Qt6::Gui Qt6::Qml PRIVATE ZLIB::ZLIB)
target_compile_options(forma_io PRIVATE -Wall -Wextra)

add_executable(forma
  packaging/forma.qrc
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
  src/app/PartDialogs.h src/app/PartDialogs.cpp
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
  src/app/PluginBrowser.h src/app/PluginBrowser.cpp
  src/plugin/FormatPlugin.h
  src/app/SelfTest.h src/app/SelfTest.cpp
)
if(WIN32)
  # The program's icon in Explorer and the task bar; no console window.
  target_sources(forma PRIVATE packaging/forma.rc)
  set_target_properties(forma PROPERTIES WIN32_EXECUTABLE ON)
endif()
target_link_libraries(forma PRIVATE forma_io Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Svg)
target_compile_definitions(forma PRIVATE FORMA_SOURCE_DIR="${CMAKE_SOURCE_DIR}" FORMA_VERSION="${PROJECT_VERSION}")
target_compile_options(forma PRIVATE -Wall -Wextra)

install(TARGETS forma RUNTIME DESTINATION bin)
# The libraries find each other beside themselves (the program finds them in ../lib).
set_target_properties(forma_core forma_io PROPERTIES INSTALL_RPATH "$ORIGIN")
install(TARGETS forma_core forma_io LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/plugin/FormatPlugin.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/forma/plugin)

# ---- plugins ----------------------------------------------------------------------
add_subdirectory(plugins/obm)
add_subdirectory(plugins/valve)
install(FILES packaging/forma.desktop DESTINATION share/applications)
install(FILES packaging/forma.png DESTINATION share/icons/hicolor/512x512/apps)

# `cmake --build build --target appimage`: packages this build as
# build/Forma-<version>-x86_64.AppImage and checks it starts with OpenGL on
# X11 and Wayland (off-screen). The same script makes the released AppImage.
if(UNIX AND NOT APPLE)
  add_custom_target(appimage
    COMMAND ${CMAKE_SOURCE_DIR}/tools/build-appimage.sh ${CMAKE_BINARY_DIR} ${PROJECT_VERSION}
    DEPENDS forma forma_obm forma_valve_vmf forma_valve_smd
    USES_TERMINAL VERBATIM)
endif()

if(FORMA_BUILD_TESTS)
  enable_testing()
  add_executable(forma_tests tests/test_core.cpp)
  target_link_libraries(forma_tests PRIVATE forma_core)
  add_test(NAME core COMMAND forma_tests)

  add_executable(forma_io_tests tests/test_io.cpp)
  target_link_libraries(forma_io_tests PRIVATE forma_io)
  add_test(NAME io COMMAND forma_io_tests)

  add_executable(forma_valve_tests tests/test_valve.cpp)
  target_link_libraries(forma_valve_tests PRIVATE forma_valve_format forma_io)
  add_test(NAME valve COMMAND forma_valve_tests)

  add_executable(forma_obm_tests tests/test_obm.cpp)
  target_link_libraries(forma_obm_tests PRIVATE forma_obm_format forma_io)
  add_test(NAME obm COMMAND forma_obm_tests)
endif()
