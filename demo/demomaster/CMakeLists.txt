# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Pull in the CANopenNode component that now lives inside this demo tree.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/canopennode")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(demomaster)

# Package the contents of demo/demomaster/storage as a SPIFFS partition image.
set(SPIFFS_IMAGE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/storage")
spiffs_create_partition_image(storage ${SPIFFS_IMAGE_SOURCE_DIR} FLASH_IN_PROJECT)
