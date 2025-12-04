cmake_minimum_required(VERSION 3.16)

# Allow greeting override via environment variable
if(DEFINED ENV{SLAVE_GREETING_OVERRIDE})
    add_compile_definitions(SLAVE_GREETING="$ENV{SLAVE_GREETING_OVERRIDE}")
endif()

# Pull in CANopenNode from the demo folder (shared dependency)
get_filename_component(DEMO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../demo/demoslave" ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${DEMO_DIR}/canopennode")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(demoslave)
