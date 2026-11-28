# Open Block Model (.obm) plugin: GNUBlocks' model format.
find_package(nlohmann_json 3 QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

# the conversion itself (also linked into the tests)
add_library(forma_obm_format STATIC ObmFormat.h ObmFormat.cpp)
set_target_properties(forma_obm_format PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(forma_obm_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forma_obm_format PUBLIC forma_io PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(forma_obm_format PRIVATE -Wall -Wextra)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_library(forma_obm MODULE ObmPlugin.cpp ObmUi.h ObmUi.cpp obm.json)
target_link_libraries(forma_obm PRIVATE forma_obm_format forma_io Qt6::Widgets)
set_target_properties(forma_obm PROPERTIES
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY ${FORMA_PLUGIN_BUILD_DIR}
  INSTALL_RPATH "$ORIGIN/../..")
target_compile_options(forma_obm PRIVATE -Wall -Wextra)
install(TARGETS forma_obm LIBRARY DESTINATION ${FORMA_PLUGIN_INSTALL_DIR})
