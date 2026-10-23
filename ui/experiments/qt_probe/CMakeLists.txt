# Qt 6 Widgets 版选型探针（与 win32_probe 同四个实验）。
# 云端交叉：cmake -S . -B /tmp/exp-qt -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/mingw-w64.cmake \
#   -DCMAKE_PREFIX_PATH=<qt>/6.8.3/mingw_64 -DQT_HOST_PATH=<qt>/6.8.3/gcc_64
# Windows 本机：cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64 && cmake --build build && windeployqt build/it2w_qt_probe.exe
cmake_minimum_required(VERSION 3.21)
project(it2w_qt_probe CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_add_executable(it2w_qt_probe WIN32 main.cpp)
target_link_libraries(it2w_qt_probe PRIVATE Qt6::Widgets)
if(WIN32)
  target_link_libraries(it2w_qt_probe PRIVATE psapi)
endif()
if(MINGW)
  target_link_options(it2w_qt_probe PRIVATE -static-libgcc -static-libstdc++)
endif()
