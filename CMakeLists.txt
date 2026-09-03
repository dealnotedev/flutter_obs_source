cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

if(NOT WIN32)
  message(FATAL_ERROR "flutter_obs_source currently supports Windows only")
endif()

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

set(
  FLUTTER_ENGINE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../flutter/engine/src/out/host_release"
  CACHE PATH "Path to a built Flutter Engine directory"
)

set(
  FLUTTER_APP_BUNDLE_DIR
  ""
  CACHE PATH "Directory containing flutter_assets, icudtl.dat and app.so"
)

set(_flutter_engine_dll "${FLUTTER_ENGINE_DIR}/flutter_engine.dll")
set(_flutter_engine_import_library "${FLUTTER_ENGINE_DIR}/flutter_engine.dll.lib")
set(_flutter_engine_header "${FLUTTER_ENGINE_DIR}/flutter_embedder.h")

foreach(_flutter_engine_file IN ITEMS
        "${_flutter_engine_dll}"
        "${_flutter_engine_import_library}"
        "${_flutter_engine_header}")
  if(NOT EXISTS "${_flutter_engine_file}")
    message(FATAL_ERROR "Required Flutter Engine artifact is missing: ${_flutter_engine_file}")
  endif()
endforeach()

add_library(Flutter::Engine SHARED IMPORTED GLOBAL)
set_target_properties(
  Flutter::Engine
  PROPERTIES
    IMPORTED_LOCATION "${_flutter_engine_dll}"
    IMPORTED_IMPLIB "${_flutter_engine_import_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${FLUTTER_ENGINE_DIR}"
)

add_library(${CMAKE_PROJECT_NAME} MODULE
        src/flutter-source.c
        src/flutter-mouse.c
        src/runtime-validation.c)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs
        Flutter::Engine
)

if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /W4)
endif()

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

install(
  FILES "${_flutter_engine_dll}"
  DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit"
)

add_custom_command(
  TARGET ${CMAKE_PROJECT_NAME}
  POST_BUILD
  COMMAND
    "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:Flutter::Engine>"
    "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:Flutter::Engine>"
    "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
  COMMENT "Copying Flutter Engine runtime next to the OBS plugin"
)

# --- miniaudio --------------------------------------------------------------
add_library(miniaudio_obj OBJECT src/miniaudio_impl.c)

target_include_directories(miniaudio_obj
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/miniaudio)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/cjson)

if (MSVC)
  target_compile_options(cjson_obj PRIVATE /std:c11)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        $<TARGET_OBJECTS:cjson_obj>)

if(FLUTTER_APP_BUNDLE_DIR)
  set(_flutter_assets "${FLUTTER_APP_BUNDLE_DIR}/flutter_assets")
  set(_flutter_icu "${FLUTTER_APP_BUNDLE_DIR}/icudtl.dat")
  set(_flutter_aot "${FLUTTER_APP_BUNDLE_DIR}/app.so")
  foreach(_flutter_runtime_item IN ITEMS "${_flutter_assets}" "${_flutter_icu}" "${_flutter_aot}")
    if(NOT EXISTS "${_flutter_runtime_item}")
      message(FATAL_ERROR "Flutter application runtime item is missing: ${_flutter_runtime_item}")
    endif()
  endforeach()

  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/flutter_obs_source"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${_flutter_assets}"
            "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/flutter_obs_source/flutter_assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_flutter_icu}"
            "${_flutter_aot}"
            "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/flutter_obs_source"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/flutter_obs_source"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${_flutter_assets}"
            "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/flutter_obs_source/flutter_assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_flutter_icu}"
            "${_flutter_aot}"
            "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/flutter_obs_source"
    COMMENT "Copying Flutter application runtime bundle"
  )

  install(DIRECTORY "${_flutter_assets}" DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit/flutter_obs_source")
  install(FILES "${_flutter_icu}" "${_flutter_aot}"
          DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit/flutter_obs_source")
else()
  message(STATUS "FLUTTER_APP_BUNDLE_DIR is not set; app.so, icudtl.dat and flutter_assets will not be packaged")
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(runtime_validation_tests
          tests/runtime-validation-test.c
          src/runtime-validation.c
          $<TARGET_OBJECTS:cjson_obj>)
  target_include_directories(runtime_validation_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  if(MSVC)
    target_compile_options(runtime_validation_tests PRIVATE /W4)
  endif()
  add_test(NAME runtime_validation_tests COMMAND runtime_validation_tests)

  add_executable(mouse_input_tests
          tests/mouse-input-test.c
          src/flutter-mouse.c)
  target_include_directories(mouse_input_tests PRIVATE
          "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${FLUTTER_ENGINE_DIR}")
  if(MSVC)
    target_compile_options(mouse_input_tests PRIVATE /W4)
  endif()
  add_test(NAME mouse_input_tests COMMAND mouse_input_tests)
endif()
