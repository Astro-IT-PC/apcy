cmake_minimum_required(VERSION 3.24)
project(apcy VERSION 0.1.0 LANGUAGES C CXX)

option(APCY_BUNDLE "macOS: build apcy as an .app bundle (used for packaging)" OFF)
option(APCY_WIN32_GUI "Windows: build without a console window" ON)
# macOS: identity passed to `codesign` when packaging. "-" is an ad-hoc signature
# (valid, but Gatekeeper still asks the user to allow the app once). Use your
# "Developer ID Application: ..." identity for distribution.
set(APCY_CODESIGN_IDENTITY "-" CACHE STRING "macOS codesign identity used when packaging")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# ---------------------------------------------------------------------------
# raylib (window, input, rendering) - static library, works on Windows/Linux/macOS
# ---------------------------------------------------------------------------
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  raylib
  URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
)

# ---------------------------------------------------------------------------
# clay (single header layout library). Header only: SOURCE_SUBDIR points at a
# directory without a CMakeLists.txt so MakeAvailable does not add its examples.
# ---------------------------------------------------------------------------
FetchContent_Declare(
  clay
  URL https://github.com/nicbarker/clay/archive/e6cc36941ab2af5d81107617039d6f527a1c660b.tar.gz
  SOURCE_SUBDIR  cmake_noop_dir
)

# ---------------------------------------------------------------------------
# nlohmann/json
# ---------------------------------------------------------------------------
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)

FetchContent_MakeAvailable(raylib clay json)

# The stock renderer uses one empty initializer `(Vector2) {}` which is C23 only
# and rejected by MSVC's C compiler; ship a patched copy from the build tree.
file(READ ${clay_SOURCE_DIR}/renderers/raylib/clay_renderer_raylib.c _clay_renderer_src)
string(REPLACE "(Vector2) {}" "(Vector2) {0}" _clay_renderer_src "${_clay_renderer_src}")
file(WRITE ${CMAKE_BINARY_DIR}/clay_vendor/clay_renderer_raylib.c "${_clay_renderer_src}")
unset(_clay_renderer_src)

# ---------------------------------------------------------------------------
# libcurl: prefer the system / vcpkg package, otherwise build it from source
# (Schannel on Windows, OpenSSL elsewhere).
# ---------------------------------------------------------------------------
find_package(CURL QUIET)
if(NOT CURL_FOUND)
  message(STATUS "libcurl not found on the system, building it from source")
  set(BUILD_CURL_EXE      OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING       OFF CACHE BOOL "" FORCE)
  set(BUILD_LIBCURL_DOCS  OFF CACHE BOOL "" FORCE)
  set(BUILD_MISC_DOCS     OFF CACHE BOOL "" FORCE)
  set(ENABLE_CURL_MANUAL  OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS   ON  CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
  set(HTTP_ONLY           ON  CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL     OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2    OFF CACHE BOOL "" FORCE)
  set(CURL_ZLIB           OFF CACHE BOOL "" FORCE)
  set(CURL_BROTLI         OFF CACHE BOOL "" FORCE)
  set(CURL_ZSTD           OFF CACHE BOOL "" FORCE)
  set(USE_NGHTTP2         OFF CACHE BOOL "" FORCE)
  set(USE_LIBIDN2         OFF CACHE BOOL "" FORCE)
  if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  elseif(APPLE)
    set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
  else()
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(
    curl
    URL https://github.com/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.xz
  )
  FetchContent_MakeAvailable(curl)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(apcy
  src/main.cpp
  src/clickup.cpp
  src/http.cpp
  src/config.cpp
  src/clay_impl.c
)

target_include_directories(apcy PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${clay_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/clay_vendor          # patched clay_renderer_raylib.c
  ${clay_SOURCE_DIR}/renderers/raylib      # raymath.h used by the renderer
)

target_link_libraries(apcy PRIVATE
  raylib
  nlohmann_json::nlohmann_json
  CURL::libcurl
  Threads::Threads
)

target_compile_definitions(apcy PRIVATE APCY_VERSION="${PROJECT_VERSION}")

if(WIN32)
  target_sources(apcy PRIVATE packaging/apcy.rc)  # executable icon
  if(APCY_WIN32_GUI)
    # GUI subsystem, but keep the plain main() entry point
    set_target_properties(apcy PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
      target_link_options(apcy PRIVATE /ENTRY:mainCRTStartup)
    endif()
  endif()
endif()

# Files the app loads at runtime (see findResource in main.cpp).
# Regenerate the logo files with: python3 packaging/make_icons.py
set(APCY_RESOURCE_FILES
  resources/Roboto-Regular.ttf
  resources/logo.png
  resources/logo-256.png
)

if(APPLE AND APCY_BUNDLE)
  set_target_properties(apcy PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "apcy"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.apcy"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "apcy.icns"
    MACOSX_BUNDLE_COPYRIGHT "MIT License")
  target_sources(apcy PRIVATE ${APCY_RESOURCE_FILES} packaging/apcy.icns)
  set_source_files_properties(${APCY_RESOURCE_FILES} packaging/apcy.icns
                              PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

if(MSVC)
  target_compile_options(apcy PRIVATE /utf-8 /W3)
  target_compile_definitions(apcy PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(apcy PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  # clay.h uses a few C99 constructs that clang/gcc warn about in C++ mode
  set_source_files_properties(src/main.cpp PROPERTIES COMPILE_OPTIONS "-Wno-missing-braces")
  # warnings inside clay.h / the stock renderer are not ours
  set_source_files_properties(src/clay_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(APPLE)
  target_link_libraries(apcy PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()

# Ship the font and an example config next to the binary (the .app bundle
# carries the font in Contents/Resources instead).
if(NOT (APPLE AND APCY_BUNDLE))
  add_custom_command(TARGET apcy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/resources $<TARGET_FILE_DIR:apcy>/resources
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config.example.json $<TARGET_FILE_DIR:apcy>/config.example.json
  )
endif()

# ---------------------------------------------------------------------------
# Install + packaging (cmake --build build --target package)
#   Windows: portable ZIP (apcy.exe + resources/ side by side)
#   Linux:   TGZ (bin/ + share/apcy/resources) and DEB
#   macOS:   DMG with apcy.app when -DAPCY_BUNDLE=ON, otherwise TGZ
# The app looks for resources next to the executable, in ../share/apcy and in
# ../Resources, so every layout above works without configuration.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
if(WIN32)
  install(TARGETS apcy RUNTIME DESTINATION . COMPONENT apcy)
  install(DIRECTORY resources DESTINATION . COMPONENT apcy)
  install(FILES config.example.json README.md DESTINATION . COMPONENT apcy)
elseif(APPLE AND APCY_BUNDLE)
  install(TARGETS apcy BUNDLE DESTINATION . COMPONENT apcy)
  install(FILES config.example.json README.md DESTINATION . COMPONENT apcy)
else()
  install(TARGETS apcy RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT apcy)
  install(DIRECTORY resources DESTINATION ${CMAKE_INSTALL_DATADIR}/apcy COMPONENT apcy)
  install(FILES config.example.json README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT apcy)
  if(NOT APPLE)
    install(FILES packaging/apcy.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT apcy)
    install(FILES resources/logo-256.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
            RENAME apcy.png COMPONENT apcy)
  endif()
endif()

# macOS: (re)sign after install. CPack strips the binary while installing, which
# invalidates the linker's ad-hoc signature, and a bundle needs a signature that
# covers its Resources too. Without this step Gatekeeper reports the downloaded
# app as "damaged". Runs after the install(TARGETS) rules above.
if(APPLE)
  if(APCY_BUNDLE)
    set(_apcy_sign_target "\${CMAKE_INSTALL_PREFIX}/apcy.app")
  else()
    set(_apcy_sign_target "\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/apcy")
  endif()
  if(APCY_CODESIGN_IDENTITY STREQUAL "-")
    set(_apcy_sign_flags --timestamp=none)
  else()
    set(_apcy_sign_flags --timestamp)
  endif()
  install(CODE "
    message(STATUS \"codesign (${APCY_CODESIGN_IDENTITY}): ${_apcy_sign_target}\")
    execute_process(
      COMMAND codesign --force --deep --options runtime ${_apcy_sign_flags}
              --sign \"${APCY_CODESIGN_IDENTITY}\" \"${_apcy_sign_target}\"
      RESULT_VARIABLE _apcy_rc)
    if(NOT _apcy_rc EQUAL 0)
      message(FATAL_ERROR \"codesign failed (\${_apcy_rc})\")
    endif()
  " COMPONENT apcy)
  unset(_apcy_sign_target)
  unset(_apcy_sign_flags)
endif()

set(CPACK_PACKAGE_NAME "apcy")
set(CPACK_PACKAGE_VENDOR "apcy")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Desktop inbox for the ClickUp tasks assigned to you or mentioning you")
set(CPACK_PACKAGE_CONTACT "acontreras@luckyintelligence.com")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_INSTALL_DIRECTORY "apcy")
# Only ship our own component; the fetched dependencies (raylib, curl) register
# install rules of their own that would otherwise end up in the package.
set(CPACK_COMPONENTS_ALL apcy)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_DEB_COMPONENT_INSTALL ON)
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_SYSTEM_NAME "win64")
elseif(APPLE)
  if(APCY_BUNDLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "apcy")
  else()
    set(CPACK_GENERATOR "TGZ")
  endif()
  set(CPACK_SYSTEM_NAME "macos-${CMAKE_HOST_SYSTEM_PROCESSOR}")
else()
  set(CPACK_GENERATOR "TGZ;DEB")
  set(CPACK_SYSTEM_NAME "linux-${CMAKE_HOST_SYSTEM_PROCESSOR}")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
  set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS
      "libcurl4 | libcurl4t64, libgl1, libx11-6, libxrandr2, libxinerama1, libxcursor1, libxi6")
endif()
include(CPack)
