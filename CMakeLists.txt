cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES C)

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# defaults.cmake downloaded OBS sources to ${CMAKE_CURRENT_SOURCE_DIR}/.deps
# and installed libobsConfig.cmake under .deps/cmake/. When the vcpkg
# toolchain is in use it can hide that prefix from find_package, so point
# directly at the config dir as a fallback.
if(NOT libobs_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.deps/cmake/libobsConfig.cmake")
  set(libobs_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/cmake" CACHE PATH "Path to libobsConfig.cmake" FORCE)
endif()
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.deps")

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

find_package(Freetype REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Freetype::Freetype)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/flametext-source.c
    src/flametext-text.c
    src/flametext-particles.c
    src/effect-registry.c
    src/effect-flame.c
    src/effect-rainbow.c
    src/effect-neon.c
    src/effect-bloom.c
    src/effect-water.c
    src/effect-glitch.c
    src/effect-watersurface.c
    src/effect-spotlight.c
    src/effect-chroma.c
    src/effect-godray.c
    src/effect-scanline.c
    src/effect-arc.c
    src/effect-circuit.c
    src/flametext-sprites.c
    src/effect-sparkle.c
    src/effect-dust.c
    src/effect-imagedeco.c
    src/effect-depth3d.c
    src/effect-cube.c
    src/effect-sidebound.c
    src/effect-hop.c
)

if(WIN32)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/flametext-font-resolve-win.c)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# --- Distribution layout (package/) ----------------------------------------
# Emit exactly the tree we ship, with the "dist" install component:
#   cmake --install <build> --config RelWithDebInfo --component dist --prefix package
#
#   package/
#   ├─ bin/
#   │   └─ font-effect-tools.dll
#   └─ font-effect-tools/
#       ├─ effects/*.effect
#       └─ locale/*.ini
install(
  TARGETS ${CMAKE_PROJECT_NAME}
  LIBRARY DESTINATION bin COMPONENT dist
  RUNTIME DESTINATION bin COMPONENT dist
)
if(MSVC)
  install(
    FILES "$<TARGET_PDB_FILE:${CMAKE_PROJECT_NAME}>"
    DESTINATION bin
    COMPONENT dist
    OPTIONAL
  )
endif()
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/effects"
  DESTINATION ${CMAKE_PROJECT_NAME}
  COMPONENT dist
)
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/locale"
  DESTINATION ${CMAKE_PROJECT_NAME}
  COMPONENT dist
)
