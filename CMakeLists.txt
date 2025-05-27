cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE
        src/flutter-source.c)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs
        F:/flutter_engine_build/flutter/engine/src/out/host_release/flutter_engine.dll.lib
)

#target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE opengl32)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/include)

# --- miniaudio --------------------------------------------------------------
add_library(miniaudio_obj OBJECT src/miniaudio_impl.c)

target_include_directories(miniaudio_obj
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)

target_compile_options(miniaudio_obj PRIVATE
        $<$<C_COMPILER_ID:MSVC>:/wd4244 /wd4334 /wd4996>
)

if (MSVC)
  target_compile_options(miniaudio_obj PRIVATE /std:c11)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:miniaudio_obj>)
# --- miniaudio --------------------------------------------------------------


# third_party/cjson/cJSON.c  third_party/cjson/cJSON.h  (MIT)
add_library(cjson_obj OBJECT src/third_party/cjson/cJSON.c)
target_include_directories(cjson_obj PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson)

if (MSVC)
  target_compile_options(cjson_obj PRIVATE /std:c11)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        $<TARGET_OBJECTS:cjson_obj>)