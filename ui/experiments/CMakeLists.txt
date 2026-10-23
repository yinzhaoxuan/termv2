# T08 外壳选型实验（独立工程，不进顶层构建）。
#   cmake -S ui/experiments -B /tmp/exp-mingw -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/mingw-w64.cmake
cmake_minimum_required(VERSION 3.20)
project(it2w_shell_experiments CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
  message(FATAL_ERROR "实验程序只针对 Windows（mingw 交叉或 MSVC）")
endif()
enable_language(RC)
set(UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. CACHE PATH "ui/ 目录（含 include/uicfg 与 src/layout_ops.cpp）")
# 只复用平台无关的布局模型（分屏 20×5 约束），不依赖 nlohmann_json
add_executable(it2w_probe WIN32 win32_probe/probe.cpp win32_probe/probe.rc
               ${UI_DIR}/src/layout_ops.cpp)
target_include_directories(it2w_probe PRIVATE ${UI_DIR}/include)
target_compile_definitions(it2w_probe PRIVATE UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(it2w_probe PRIVATE imm32 comctl32 psapi user32 gdi32 shell32 ntdll)
if(MINGW)
  target_compile_options(it2w_probe PRIVATE -Wall -Wextra -municode)
  target_link_options(it2w_probe PRIVATE -municode -static -static-libgcc -static-libstdc++)
elseif(MSVC)
  target_compile_options(it2w_probe PRIVATE /utf-8 /W4)
endif()
