# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.4.1")
add_definitions(-DFIRMWARE_VERSION=\"${PROJECT_VER}\")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Auto-load a local sdkconfig overlay (git-ignored) when present. Lets
# self-hosted / custom deployments pin CONFIG_STACKCHAN_SERVER_URL,
# CONFIG_OTA_URL, etc. without touching committed defaults.
# Manual equivalent: SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" idf.py build
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local"
        CACHE STRING "ESP-IDF sdkconfig default files (auto-extended with local overlay)" FORCE)
    message(STATUS "StackChan: detected sdkconfig.defaults.local, applying overlay")
endif()

# MicroLink (Tailscale client) lives outside components/ so its repo-root
# CMakeLists isn't mistaken for an IDF component. Point at its components/ dir;
# the nested wireguard_lwip component is auto-discovered (same as MicroLink's own
# examples). Fetched by fetch_repos.py into firmware/microlink (git-ignored), so
# the guard keeps the build working before the dependency is fetched.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/microlink/components")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/microlink/components")
    message(STATUS "StackChan: MicroLink (Tailscale) component enabled")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stack-chan)
