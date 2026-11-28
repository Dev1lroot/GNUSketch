# Valve Source engine exports: Hammer maps (.vmf) and studio models (.smd).
add_library(forma_valve_format STATIC ValveFormat.h ValveFormat.cpp)
set_target_properties(forma_valve_format PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(forma_valve_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forma_valve_format PUBLIC forma_io)
target_compile_options(forma_valve_format PRIVATE -Wall -Wextra)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
foreach(kind vmf smd)
  if(kind STREQUAL "vmf")
    set(src VmfPlugin.cpp)
  else()
    set(src SmdPlugin.cpp)
  endif()
  add_library(forma_valve_${kind} MODULE ${src} ${kind}.json)
  target_link_libraries(forma_valve_${kind} PRIVATE forma_valve_format forma_io Qt6::Widgets)
  set_target_properties(forma_valve_${kind} PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${FORMA_PLUGIN_BUILD_DIR}
    INSTALL_RPATH "$ORIGIN/../..")
  target_compile_options(forma_valve_${kind} PRIVATE -Wall -Wextra)
  install(TARGETS forma_valve_${kind} LIBRARY DESTINATION ${FORMA_PLUGIN_INSTALL_DIR})
endforeach()
