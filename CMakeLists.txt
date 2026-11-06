# Hekpixle — C++ desktop agent (WebView2 host + C++ backend, renderer unchanged).
# Windows + MSVC only. No vcpkg and no git needed: the two dependencies are
#   - nlohmann/json      -> single header, shipped as nlohmann_json.hpp
#   - WebView2 SDK       -> downloaded from nuget.org at configure time (needs internet once)
cmake_minimum_required(VERSION 3.20)
project(Hekpixle VERSION 0.13.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Hekpixle C++ build targets Windows (WebView2 + Win32).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: no VC++ redistributable needed

# One fixed output folder for every generator/config: build/out/
set(HEK_OUT ${CMAKE_BINARY_DIR}/out)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY         ${HEK_OUT})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${HEK_OUT})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${HEK_OUT})

# ---------------- nlohmann/json (vendored as nlohmann_json.hpp) ----------------
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/nlohmann_json.hpp")
  message(STATUS "Downloading nlohmann/json...")
  file(DOWNLOAD "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
       "${CMAKE_SOURCE_DIR}/nlohmann_json.hpp" TLS_VERIFY ON STATUS dl_status)
  list(GET dl_status 0 dl_code)
  if(NOT dl_code EQUAL 0)
    message(FATAL_ERROR "Could not download nlohmann/json.hpp. Save it manually as nlohmann_json.hpp next to CMakeLists.txt")
  endif()
endif()

# ---------------- WebView2 SDK (NuGet) ----------------
set(WV2_VERSION 1.0.2739.15)
set(WV2_DIR ${CMAKE_BINARY_DIR}/webview2-${WV2_VERSION})
if(NOT EXISTS "${WV2_DIR}/build/native/include/WebView2.h")
  message(STATUS "Downloading Microsoft.Web.WebView2 ${WV2_VERSION} from nuget.org ...")
  set(WV2_ZIP ${CMAKE_BINARY_DIR}/webview2.zip)
  file(DOWNLOAD "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WV2_VERSION}"
       "${WV2_ZIP}" TLS_VERIFY ON STATUS dl_status)
  list(GET dl_status 0 dl_code)
  if(NOT dl_code EQUAL 0)
    message(FATAL_ERROR "Could not download the WebView2 SDK (${dl_status}). Check your internet connection / VPN and run again.")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${WV2_ZIP}" DESTINATION "${WV2_DIR}")
endif()
if(NOT EXISTS "${WV2_DIR}/build/native/x64/WebView2Loader.dll.lib")
  message(FATAL_ERROR "WebView2 SDK extracted but WebView2Loader.dll.lib is missing in ${WV2_DIR}/build/native/x64")
endif()

add_executable(Hekpixle WIN32
  main.cpp window.cpp ipc.cpp ipc_shim.cpp agent.cpp context.cpp util.cpp
  web.cpp files.cpp computer.cpp browser.cpp mcp.cpp media.cpp procs.cpp ps.cpp
  skills.cpp cron.cpp plugins.cpp agent_tools.cpp telegram.cpp
)

target_include_directories(Hekpixle PRIVATE
  ${CMAKE_SOURCE_DIR}                      # all headers live next to the sources
  ${WV2_DIR}/build/native/include          # <WebView2.h>
)
target_compile_definitions(Hekpixle PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(Hekpixle PRIVATE /utf-8 /EHsc /bigobj /W3)

target_link_libraries(Hekpixle PRIVATE
  ${WV2_DIR}/build/native/x64/WebView2Loader.dll.lib
  winhttp shcore dwmapi gdi32 user32 shell32 shlwapi ole32 oleaut32 advapi32 psapi winmm uuid gdiplus comdlg32 version
)

# Icon + version info (rc template) and the application manifest (DPI, UTF-8, common controls)
configure_file(${CMAKE_SOURCE_DIR}/Hekpixle.rc.in ${CMAKE_BINARY_DIR}/Hekpixle.rc @ONLY)
target_sources(Hekpixle PRIVATE ${CMAKE_BINARY_DIR}/Hekpixle.rc ${CMAKE_SOURCE_DIR}/Hekpixle.manifest)

# ---------------------------------------------------------------------------------------
# The repository is FLAT (GitHub's web upload cannot create folders). The app expects its
# original folder layout next to the exe, so after the build we copy the flat files into
#   out/renderer/ (+fonts, lib/katex, lib/highlight), out/assets/, out/skills_seed/<name>/SKILL.md
# ---------------------------------------------------------------------------------------
function(hek_copy_group dest)
  set(files "")
  foreach(pattern ${ARGN})
    file(GLOB matched "${CMAKE_SOURCE_DIR}/${pattern}")
    list(APPEND files ${matched})
  endforeach()
  add_custom_command(TARGET Hekpixle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HEK_OUT}/${dest}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${files} "${HEK_OUT}/${dest}")
endfunction()

hek_copy_group(renderer                    index.html app.js styles.css)
hek_copy_group(renderer/fonts              Vazirmatn-*.woff2 SpaceGrotesk-*.woff2)
hek_copy_group(renderer/lib                marked.min.js)
hek_copy_group(renderer/lib/katex          katex.min.css katex.min.js)
hek_copy_group(renderer/lib/katex/fonts    KaTeX_*.woff2)
hek_copy_group(renderer/lib/katex/contrib  auto-render.min.js)
hek_copy_group(renderer/lib/highlight      highlight.min.js github.min.css github-dark.min.css)
hek_copy_group(assets                      icon.png icon.ico icon.svg icon-512.png)

foreach(skill web-research computer-use creating-skills)
  add_custom_command(TARGET Hekpixle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HEK_OUT}/skills_seed/${skill}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/skill-${skill}.md" "${HEK_OUT}/skills_seed/${skill}/SKILL.md")
endforeach()

add_custom_command(TARGET Hekpixle POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WV2_DIR}/build/native/x64/WebView2Loader.dll" "${HEK_OUT}/WebView2Loader.dll")
