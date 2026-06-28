cmake_minimum_required(VERSION 3.5)
project(jsapi_browser C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IOT_UI_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/iot-miniapp-sdk")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}")
include_directories("${IOT_UI_SDK_PATH}/include")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BUILD_FLAGS "-Wall -Werror=return-type -g -O0")
else()
    set(BUILD_FLAGS "-Wall -Werror=return-type -Os -g")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${BUILD_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${BUILD_FLAGS}")
set(CMAKE_SKIP_BUILD_RPATH true)

add_definitions(-D__DEBUG -DHAASUI_LOG_TO_SYSLOG)

file(GLOB_RECURSE SDK_SOURCES "${IOT_UI_SDK_PATH}/src/*.cpp")
file(GLOB_RECURSE BROWSER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(jsapi_browser SHARED ${SDK_SOURCES} ${BROWSER_SOURCES})
target_include_directories(jsapi_browser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/jsapi_browser")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(jsapi_browser dl pthread -Wl,-unresolved-symbols=ignore-all)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(jsapi_browser dl pthread -Wl,-undefined,dynamic_lookup)
else()
    target_link_libraries(jsapi_browser dl pthread)
endif()

set(CMAKE_VERBOSE_MAKEFILE ON)
