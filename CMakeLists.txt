cmake_minimum_required(VERSION 3.28)

project(propresenter-lyrics VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT DEFINED SIMDe_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shims/SIMDe/SIMDeConfig.cmake")
  set(SIMDe_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shims/SIMDe" CACHE PATH "SIMDe CMake package directory")
endif()

if(NOT DEFINED libobs_DIR)
  set(_propresenter_lyrics_libobs_candidates)

  if(DEFINED ENV{LIBOBS_DIR})
    list(APPEND _propresenter_lyrics_libobs_candidates "$ENV{LIBOBS_DIR}")
  endif()

  if(DEFINED ENV{LIBOBS_CONFIG})
    get_filename_component(_propresenter_lyrics_env_libobs_dir "$ENV{LIBOBS_CONFIG}" DIRECTORY)
    list(APPEND _propresenter_lyrics_libobs_candidates "${_propresenter_lyrics_env_libobs_dir}")
  endif()

  if(DEFINED ENV{HOME})
    file(GLOB _propresenter_lyrics_sdk_roots
      "$ENV{HOME}/Developer/obs-sdk*"
    )
    list(SORT _propresenter_lyrics_sdk_roots ORDER DESCENDING)
    list(APPEND _propresenter_lyrics_libobs_candidates
      ${_propresenter_lyrics_sdk_roots}
      "$ENV{HOME}/Developer/obs-studio/build_macos/libobs"
      "$ENV{HOME}/Developer/obs-studio/build/libobs"
      "$ENV{HOME}/Developer/obs-studio"
    )
  endif()

  list(APPEND _propresenter_lyrics_libobs_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/../obs-sdk"
    "${CMAKE_CURRENT_SOURCE_DIR}/../obs-studio/build_macos/libobs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../obs-studio/build/libobs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../obs-studio"
  )

  foreach(_propresenter_lyrics_candidate IN LISTS _propresenter_lyrics_libobs_candidates)
    if(EXISTS "${_propresenter_lyrics_candidate}/libobsConfig.cmake" OR EXISTS "${_propresenter_lyrics_candidate}/libobs-config.cmake")
      set(libobs_DIR "${_propresenter_lyrics_candidate}" CACHE PATH "libobs CMake package directory")
      break()
    elseif(EXISTS "${_propresenter_lyrics_candidate}/lib/cmake/libobs/libobsConfig.cmake")
      set(libobs_DIR "${_propresenter_lyrics_candidate}/lib/cmake/libobs" CACHE PATH "libobs CMake package directory")
      break()
    elseif(EXISTS "${_propresenter_lyrics_candidate}/build_macos/libobs/libobsConfig.cmake")
      set(libobs_DIR "${_propresenter_lyrics_candidate}/build_macos/libobs" CACHE PATH "libobs CMake package directory")
      break()
    elseif(EXISTS "${_propresenter_lyrics_candidate}/build/libobs/libobsConfig.cmake")
      set(libobs_DIR "${_propresenter_lyrics_candidate}/build/libobs" CACHE PATH "libobs CMake package directory")
      break()
    endif()
  endforeach()
endif()

if(DEFINED libobs_DIR)
  message(STATUS "Using libobs_DIR=${libobs_DIR}")
endif()

find_package(libobs REQUIRED CONFIG)

# Tabs add an enum value, not a new exported function. Keep the baseline SDK
# so the plugin can still load in older OBS versions and display normal groups.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES OBS::libobs)
check_cxx_source_compiles("#include <obs-properties.h>
int main() { return static_cast<int>(OBS_GROUP_TAB); }" PROPRESENTER_HAS_OBS_GROUP_TAB)
unset(CMAKE_REQUIRED_LIBRARIES)
configure_file(src/obs-tabs-compat.hpp.in generated/obs-tabs-compat.hpp)

add_library(propresenter-lyrics MODULE
  src/propresenter-lyrics.cpp
)

target_link_libraries(propresenter-lyrics PRIVATE OBS::libobs)
target_include_directories(propresenter-lyrics PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(WIN32)
  target_link_libraries(propresenter-lyrics PRIVATE ws2_32)
endif()

target_compile_definitions(propresenter-lyrics PRIVATE
  PROJECT_VERSION="${PROJECT_VERSION}"
)

set_target_properties(propresenter-lyrics PROPERTIES
  PREFIX ""
  OUTPUT_NAME "propresenter-lyrics"
)

if(APPLE)
  set_target_properties(propresenter-lyrics PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "plugin"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "ProPresenter Lyrics"
    MACOSX_BUNDLE_BUNDLE_IDENTIFIER "com.reginald.obs-propresenter-lyrics"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  )
endif()

if(WIN32)
  install(TARGETS propresenter-lyrics
    RUNTIME DESTINATION "propresenter-lyrics/bin/64bit"
    LIBRARY DESTINATION "propresenter-lyrics/bin/64bit"
  )
  install(DIRECTORY data/ DESTINATION "propresenter-lyrics/data")
  install(FILES icon/plugin-icon.svg DESTINATION "propresenter-lyrics/data/icon")
elseif(APPLE)
  install(TARGETS propresenter-lyrics
    BUNDLE DESTINATION "."
    LIBRARY DESTINATION "."
  )
  install(DIRECTORY data/locale DESTINATION "propresenter-lyrics.plugin/Contents/Resources")
  install(FILES icon/plugin-icon.svg DESTINATION "propresenter-lyrics.plugin/Contents/Resources/icon")
else()
  install(TARGETS propresenter-lyrics
    LIBRARY DESTINATION "propresenter-lyrics/bin/64bit"
  )
  install(DIRECTORY data/ DESTINATION "propresenter-lyrics/data")
  install(FILES icon/plugin-icon.svg DESTINATION "propresenter-lyrics/data/icon")
endif()

option(PROPRESENTER_BUILD_TESTS "Build standalone network regression tests" OFF)
if(PROPRESENTER_BUILD_TESTS)
  enable_testing()
  add_executable(stage-client-test tests/stage-client.cpp)
  target_link_libraries(stage-client-test PRIVATE OBS::libobs)
  target_include_directories(stage-client-test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_compile_definitions(stage-client-test PRIVATE PROJECT_VERSION="${PROJECT_VERSION}")
  add_executable(stage-discovery-test tests/stage-discovery.cpp)
  if(WIN32)
    target_link_libraries(stage-client-test PRIVATE ws2_32)
    target_link_libraries(stage-discovery-test PRIVATE ws2_32)
  endif()
  # These checks use assertions even in release builds.
  if(MSVC)
    target_compile_options(stage-client-test PRIVATE /UNDEBUG)
    target_compile_options(stage-discovery-test PRIVATE /UNDEBUG)
  else()
    target_compile_options(stage-client-test PRIVATE -UNDEBUG)
    target_compile_options(stage-discovery-test PRIVATE -UNDEBUG)
  endif()
  add_test(NAME stage-client-responsiveness COMMAND stage-client-test)
  add_test(NAME stage-discovery COMMAND stage-discovery-test)
  set_tests_properties(stage-client-responsiveness stage-discovery PROPERTIES TIMEOUT 30)
endif()
