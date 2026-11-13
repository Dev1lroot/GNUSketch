# Valve Source engine exports: Hammer maps (.vmf) and studio models (.smd).
add_library(gs_valve_format STATIC ValveFormat.h ValveFormat.cpp)
set_target_properties(gs_valve_format PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(gs_valve_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gs_valve_format PUBLIC gs_io)
target_compile_options(gs_valve_format PRIVATE -Wall -Wextra)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
foreach(kind vmf smd)
  if(kind STREQUAL "vmf")
    set(src VmfPlugin.cpp)
  else()
    set(src SmdPlugin.cpp)
  endif()
  add_library(gnusketch_valve_${kind} MODULE ${src} ${kind}.json)
  target_link_libraries(gnusketch_valve_${kind} PRIVATE gs_valve_format gs_io Qt6::Widgets)
  set_target_properties(gnusketch_valve_${kind} PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${GNUSKETCH_PLUGIN_BUILD_DIR}
    INSTALL_RPATH "$ORIGIN/../..")
  target_compile_options(gnusketch_valve_${kind} PRIVATE -Wall -Wextra)
  install(TARGETS gnusketch_valve_${kind} LIBRARY DESTINATION ${GNUSKETCH_PLUGIN_INSTALL_DIR})
endforeach()
