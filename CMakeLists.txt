cmake_minimum_required(VERSION 3.20)
project(CrunchCord LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- Dear ImGui (fetched) --------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.5
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)
set(IMGUI_DIR ${imgui_SOURCE_DIR})

# ---- App -------------------------------------------------------------------
add_executable(CrunchCord
  src/main.cpp
  src/compressor.cpp
  src/process.cpp
  src/dialogs.cpp
  src/clipboard.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(CrunchCord PRIVATE
  src
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
)

target_compile_definitions(CrunchCord PRIVATE
  UNICODE _UNICODE
  NOMINMAX
)

target_link_libraries(CrunchCord PRIVATE
  d3d11 d3dcompiler dxgi dwmapi shell32 ole32 uuid gdiplus
)

if(MSVC)
  target_compile_definitions(CrunchCord PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(CrunchCord PRIVATE /utf-8 /W3 /EHsc)
  # Static CRT so the released .exe runs standalone (no VC++ redistributable).
  set_property(TARGET CrunchCord PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # GUI subsystem (no console window) while keeping a standard int main()
  set_target_properties(CrunchCord PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
else()
  target_compile_options(CrunchCord PRIVATE -Wall)
  # -mwindows: no console window.  -static: bundle the MinGW runtime so the
  # exe runs standalone when double-clicked from Explorer.
  target_link_options(CrunchCord PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
endif()
