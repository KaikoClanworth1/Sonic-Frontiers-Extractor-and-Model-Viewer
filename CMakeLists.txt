cmake_minimum_required(VERSION 3.16)
project(SonicFrontiersViewer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Static CRT so the exe runs without VC++ redistributables.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /MP /wd4996 /wd4244 /wd4267)
endif()

set(EXT ${CMAKE_SOURCE_DIR}/extern)

# ---------------- third-party ----------------
add_library(glad STATIC ${EXT}/glad/src/gl.c)
target_include_directories(glad PUBLIC ${EXT}/glad/include)

add_library(lz4 STATIC ${EXT}/lz4/lz4.c)
target_include_directories(lz4 PUBLIC ${EXT}/lz4)

add_library(imgui STATIC
  ${EXT}/imgui/imgui.cpp
  ${EXT}/imgui/imgui_draw.cpp
  ${EXT}/imgui/imgui_tables.cpp
  ${EXT}/imgui/imgui_widgets.cpp
  ${EXT}/imgui/backends/imgui_impl_glfw.cpp
  ${EXT}/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${EXT}/imgui ${EXT}/imgui/backends ${EXT}/glfw/include)

add_library(tinyfd STATIC ${EXT}/tinyfd/tinyfiledialogs.c)
target_include_directories(tinyfd PUBLIC ${EXT}/tinyfd)

# GLFW prebuilt static lib (vc2019) — glfw3_mt.lib matches the static CRT (/MT).
set(GLFW_LIB ${EXT}/glfw/lib-vc2019/glfw3_mt.lib)

# ---------------- core parser library ----------------
file(GLOB CORE_SRC ${CMAKE_SOURCE_DIR}/src/core/*.cpp)
add_library(sfcore STATIC ${CORE_SRC})
target_include_directories(sfcore PUBLIC ${CMAKE_SOURCE_DIR}/src/core ${EXT}/stb ${EXT}/lz4)
target_link_libraries(sfcore PUBLIC lz4)

# ---------------- CLI test harness ----------------
add_executable(sfcli ${CMAKE_SOURCE_DIR}/src/cli/sfcli.cpp)
target_link_libraries(sfcli PRIVATE sfcore)

# ---------------- GUI app ----------------
file(GLOB APP_SRC ${CMAKE_SOURCE_DIR}/src/app/*.cpp)
add_executable(SonicFrontiersViewer ${APP_SRC})
target_include_directories(SonicFrontiersViewer PRIVATE ${CMAKE_SOURCE_DIR}/src/core ${EXT}/stb)
target_link_libraries(SonicFrontiersViewer PRIVATE sfcore imgui glad tinyfd ${GLFW_LIB}
  opengl32 gdi32 shell32 user32 comdlg32 ole32)
# console for debugging when SFV_DEBUG set; keep WIN32 (no console window) for release
