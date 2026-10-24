# Port launcher: one app for every port, driven by the game's launcher.toml (pages, options, saves, executable).
# SDL3 for the window, the renderer, controllers, dialogs and processes; Dear ImGui for the interface.

cmake_minimum_required(VERSION 3.25)
project(port_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9b GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3 tomlplusplus imgui)

file(GLOB LAUNCHER_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(Launcher WIN32 ${LAUNCHER_SOURCES}
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(Launcher PRIVATE src third_party ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(Launcher PRIVATE SDL3::SDL3-static tomlplusplus::tomlplusplus)
set_target_properties(Launcher PROPERTIES OUTPUT_NAME "Otavix Launcher")
if(WIN32)
    target_compile_definitions(Launcher PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_link_libraries(Launcher PRIVATE winhttp dxgi crypt32 bcrypt)
endif()

# fonts beside the executable; the profile is the game's (copied as launcher.toml by the game's packaging)
add_custom_command(TARGET Launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts $<TARGET_FILE_DIR:Launcher>/launcher/fonts)
