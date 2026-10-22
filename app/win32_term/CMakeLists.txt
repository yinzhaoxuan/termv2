# T06 最小 Win32 终端实验窗（仅 Windows / mingw 交叉编译）
if(NOT WIN32)
  message(STATUS "跳过 app/win32_term（非 WIN32）")
  return()
endif()

add_executable(win32_term
  win32_term.cpp
)
target_include_directories(win32_term PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(win32_term PRIVATE
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(win32_term PRIVATE
  itcore
  it2w_conpty
  itrender_d2d
  itrender_layout
  imm32
  user32
  gdi32
)
if(MINGW)
  target_compile_options(win32_term PRIVATE -Wall -Wextra)
  # -municode → wWinMain；保留 console 子系统便于 stderr 诊断
  target_link_options(win32_term PRIVATE -municode -static)
endif()
if(MSVC)
  target_link_options(win32_term PRIVATE /ENTRY:wWinMainCRTStartup)
endif()
