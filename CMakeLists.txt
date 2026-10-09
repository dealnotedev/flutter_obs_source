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

add_library(flutter_renderer STATIC
        src/flutter-renderer.cpp
        src/flutter-renderer-software.cpp
        src/flutter-renderer-gpu.cpp)

add_library(${CMAKE_PROJECT_NAME} MODULE
        src/flutter-source.c
        src/flutter-mouse.c
        src/flutter-keyboard.c
        src/flutter-text-input.c
        src/runtime-validation.c)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs
        Flutter::Engine
        flutter_renderer
)

set(FLUTTER_ANGLE_DIR "${FLUTTER_ENGINE_DIR}" CACHE PATH "Directory containing the ANGLE runtime DLLs")
set(FLUTTER_ANGLE_INCLUDE_DIR "${FLUTTER_ENGINE_DIR}/../../flutter/third_party/angle/include"
    CACHE PATH "Directory containing the ANGLE EGL and GLES headers")
if(NOT EXISTS "${FLUTTER_ANGLE_INCLUDE_DIR}/EGL/eglext_angle.h")
  message(FATAL_ERROR "Set FLUTTER_ANGLE_INCLUDE_DIR to the ANGLE include directory")
endif()
target_include_directories(flutter_renderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${FLUTTER_ENGINE_DIR}"
                           PRIVATE "${FLUTTER_ANGLE_INCLUDE_DIR}")
target_link_libraries(flutter_renderer PUBLIC OBS::libobs PRIVATE d3d11 dxgi)
target_compile_features(flutter_renderer PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(flutter_renderer PRIVATE /W4 /EHsc)
endif()

# ANGLE is loaded only by the GPU source, from its own runtime directory.
foreach(_angle_dll IN ITEMS libEGL.dll libGLESv2.dll d3dcompiler_47.dll vulkan-1.dll)
  if(EXISTS "${FLUTTER_ANGLE_DIR}/${_angle_dll}")
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/flutter_obs_source/angle"
              "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/flutter_obs_source/angle"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${FLUTTER_ANGLE_DIR}/${_angle_dll}"
              "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/flutter_obs_source/angle"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${FLUTTER_ANGLE_DIR}/${_angle_dll}"
              "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/flutter_obs_source/angle")
    install(FILES "${FLUTTER_ANGLE_DIR}/${_angle_dll}"
            DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit/flutter_obs_source/angle")
  elseif(_angle_dll MATCHES "^lib(EGL|GLESv2)")
    message(WARNING "${_angle_dll} is missing; the GPU source needs this ANGLE runtime DLL")
  endif()
endforeach()

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
  add_executable(renderer_transfer_tests tests/renderer-transfer-test.cpp)
  target_link_libraries(renderer_transfer_tests PRIVATE flutter_renderer)
  add_dependencies(renderer_transfer_tests ${CMAKE_PROJECT_NAME})
  target_include_directories(renderer_transfer_tests PRIVATE "${FLUTTER_ANGLE_INCLUDE_DIR}")
  if(MSVC)
    target_compile_options(renderer_transfer_tests PRIVATE /W4 /EHsc)
  endif()
  set(OBS_RENDERER_TEST_BIN "" CACHE PATH "OBS bin/64bit directory for real GPU transfer tests")
  set(OBS_RENDERER_TEST_DATA "" CACHE PATH "OBS data/libobs directory for real GPU transfer tests")
  set(OBS_RENDERER_TEST_ANGLE_DIR "" CACHE PATH "Existing OBS ANGLE directory for DLL coexistence tests")
  if(OBS_RENDERER_TEST_BIN AND OBS_RENDERER_TEST_DATA)
    add_test(NAME renderer_transfer_tests COMMAND renderer_transfer_tests
             "${OBS_RENDERER_TEST_BIN}/libobs-d3d11.dll" "${OBS_RENDERER_TEST_DATA}"
             "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>")
    set_tests_properties(renderer_transfer_tests PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_RENDERER_TEST_BIN}"
      TIMEOUT 60)
    set(FLUTTER_RENDERER_TEST_BUNDLE "" CACHE PATH "Offline Flutter AOT fixture bundle for engine raster tests")
    if(FLUTTER_RENDERER_TEST_BUNDLE)
      # Exercise source creation with the offline fixture, leaving the production bundle intact.
      set(_renderer_test_runtime "${CMAKE_CURRENT_BINARY_DIR}/renderer-source-runtime")
      add_custom_target(renderer_test_runtime ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_renderer_test_runtime}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
                "$<TARGET_FILE:Flutter::Engine>" "${_renderer_test_runtime}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${FLUTTER_RENDERER_TEST_BUNDLE}"
                "${_renderer_test_runtime}/flutter_obs_source"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/flutter_obs_source/angle"
                "${_renderer_test_runtime}/flutter_obs_source/angle"
        DEPENDS ${CMAKE_PROJECT_NAME})
      add_dependencies(renderer_transfer_tests renderer_test_runtime)
      add_test(NAME renderer_engine_tests COMMAND renderer_transfer_tests
               "${OBS_RENDERER_TEST_BIN}/libobs-d3d11.dll" "${OBS_RENDERER_TEST_DATA}"
               "${_renderer_test_runtime}/flutter_obs_source.dll" --engine)
      set_tests_properties(renderer_engine_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_RENDERER_TEST_BIN}" TIMEOUT 60)
    endif()
    if(OBS_RENDERER_TEST_ANGLE_DIR)
      set(_coexistence_plugin "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>")
      set(_coexistence_engine_args)
      if(FLUTTER_RENDERER_TEST_BUNDLE)
        set(_coexistence_plugin "${_renderer_test_runtime}/flutter_obs_source.dll")
        set(_coexistence_engine_args --engine)
      endif()
      add_test(NAME renderer_angle_coexistence_tests COMMAND renderer_transfer_tests
               "${OBS_RENDERER_TEST_BIN}/libobs-d3d11.dll" "${OBS_RENDERER_TEST_DATA}"
               "${_coexistence_plugin}" ${_coexistence_engine_args}
               --preload-angle "${OBS_RENDERER_TEST_ANGLE_DIR}")
      set_tests_properties(renderer_angle_coexistence_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_RENDERER_TEST_BIN}" TIMEOUT 60)
    endif()
  endif()
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

  add_executable(keyboard_input_tests
          tests/keyboard-input-test.c
          src/flutter-keyboard.c
          src/flutter-text-input.c
          $<TARGET_OBJECTS:cjson_obj>)
  target_include_directories(keyboard_input_tests PRIVATE
          "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${FLUTTER_ENGINE_DIR}")
  target_link_libraries(keyboard_input_tests PRIVATE user32)
  if(MSVC)
    target_compile_options(keyboard_input_tests PRIVATE /W4)
  endif()
  add_test(NAME keyboard_input_tests COMMAND keyboard_input_tests)
endif()
