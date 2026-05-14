# set(EXTRA_COMPONENT_DIRS components/esp-sr)
# cmake_minimum_required(VERSION 3.16)

# include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# project(SentinelProject)

cmake_minimum_required(VERSION 3.16)

# Tell the build system where the esp-sr folder is
list(APPEND EXTRA_COMPONENT_DIRS "components/esp-sr")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(SentinelProject)
