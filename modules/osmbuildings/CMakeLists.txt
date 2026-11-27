include(${PROJECT_SOURCE_DIR}/support/cmake/module_definition.cmake)

set(HEADER_FILES
    osmbuildingsmodule.h
    rendering/renderableosmbuildings.h
)

set(SOURCE_FILES
    osmbuildingsmodule.cpp
    rendering/renderableosmbuildings.cpp
)

set(SHADER_FILES
    shaders/buildings_vs.glsl
    shaders/buildings_fs.glsl
)

create_new_module(
    "OsmBuildings"
    osmbuildings_module
    ${HEADER_FILES} ${SOURCE_FILES} ${SHADER_FILES}
)

find_package(CURL REQUIRED)
target_link_libraries(openspace-module-osmbuildings PRIVATE CURL::libcurl)