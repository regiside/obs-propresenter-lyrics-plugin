# Homebrew's simde formula can provide headers without a CMake package file.
# OBS exports SIMDe as a public dependency, so this shim supplies the imported
# interface target expected by libobsConfig.cmake.

set(SIMDe_FOUND TRUE)

if(NOT DEFINED SIMDe_INCLUDE_DIR)
  if(EXISTS "/opt/homebrew/include/simde")
    set(SIMDe_INCLUDE_DIR "/opt/homebrew/include")
  elseif(EXISTS "/usr/local/include/simde")
    set(SIMDe_INCLUDE_DIR "/usr/local/include")
  endif()
endif()

if(NOT SIMDe_INCLUDE_DIR OR NOT EXISTS "${SIMDe_INCLUDE_DIR}/simde")
  set(SIMDe_FOUND FALSE)
  if(SIMDe_FIND_REQUIRED)
    message(FATAL_ERROR "SIMDe headers were not found. Install them with: brew install simde")
  endif()
  return()
endif()

if(NOT TARGET SIMDe::SIMDe)
  add_library(SIMDe::SIMDe INTERFACE IMPORTED)
  set_target_properties(SIMDe::SIMDe PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SIMDe_INCLUDE_DIR}"
  )
endif()

