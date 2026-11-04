# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(uicfg CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 原生、交叉及独立 ui 构建默认共用仓库内头文件；系统包仅显式启用。
option(UICFG_USE_SYSTEM_NLOHMANN "Use the system nlohmann_json package" OFF)
set(UICFG_NLOHMANN_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann_json/include"
    CACHE PATH "header-only nlohmann/json include dir")
if(UICFG_USE_SYSTEM_NLOHMANN)
  find_package(nlohmann_json 3.11 REQUIRED)
endif()
add_library(uicfg src/diag_bundle.cpp src/validate.cpp src/store.cpp src/connection_form.cpp src/launch.cpp src/snapshot_cleanup.cpp src/known_hosts.cpp src/workspace.cpp src/settings.cpp src/keybinding_view.cpp src/appearance.cpp src/itermcolors.cpp src/layout_ops.cpp src/link_policy.cpp src/close_confirm.cpp src/tab_model.cpp src/session_tree.cpp src/reconnect.cpp src/shell_logic.cpp src/shell_models.cpp)
target_include_directories(uicfg PUBLIC include)
target_include_directories(uicfg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../app/common)
if(UICFG_USE_SYSTEM_NLOHMANN)
  target_link_libraries(uicfg PUBLIC nlohmann_json::nlohmann_json)
else()
  target_include_directories(uicfg SYSTEM PUBLIC ${UICFG_NLOHMANN_INCLUDE_DIR})
endif()
if(MSVC)
  target_compile_options(uicfg PRIVATE /utf-8 /W4)
endif()
enable_testing()
foreach(t validate_tests store_tests launch_tests compose_environment_tests snapshot_cleanup_tests known_hosts_tests workspace_tests settings_tests layout_ops_tests link_policy_tests close_confirm_tests tab_model_tests session_tree_tests reconnect_tests shell_logic_tests shell_models_tests)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} uicfg)
  add_test(NAME uicfg_${t} COMMAND ${t})
endforeach()

# DS-10：剪贴板读确认文案（纯函数）。
add_executable(clipboard_prompt_tests tests/clipboard_prompt_tests.cpp)
target_link_libraries(clipboard_prompt_tests PRIVATE uicfg)
foreach(mode empty short truncation controls local)
  add_test(NAME uicfg_clipboard_prompt_${mode} COMMAND clipboard_prompt_tests ${mode})
endforeach()

# DS-14：表单与编辑操作按模式单独登记。
add_executable(connection_form_tests tests/connection_form_tests.cpp)
target_link_libraries(connection_form_tests PRIVATE uicfg)
foreach(mode roundtrip fields creation groups menus prompts stamps arguments)
  add_test(NAME uicfg_connection_form_${mode} COMMAND connection_form_tests ${mode})
endforeach()

# DS-13：存储、校验与两种 SSH 启动路径分别登记。
add_executable(ssh_transport_tests tests/ssh_transport_tests.cpp)
target_link_libraries(ssh_transport_tests PRIVATE uicfg)
foreach(mode store validate launch)
  add_test(NAME uicfg_ssh_transport_${mode} COMMAND ssh_transport_tests ${mode})
endforeach()

# DS-9：可移植外观规则与手写 iTerm2 XML 样本。
add_executable(appearance_tests tests/appearance_tests.cpp)
target_link_libraries(appearance_tests PRIVATE uicfg)
target_include_directories(appearance_tests PRIVATE ../renderer/layout ../ports/core/include)
foreach(mode settings resolution mutation)
  add_test(NAME uicfg_appearance_${mode} COMMAND appearance_tests ${mode})
endforeach()
add_executable(itermcolors_tests tests/itermcolors_tests.cpp)
target_link_libraries(itermcolors_tests PRIVATE uicfg)
target_compile_definitions(itermcolors_tests PRIVATE ITERM_COLORS_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/handmade.itermcolors")
foreach(mode sample parsing prompt)
  add_test(NAME uicfg_itermcolors_${mode} COMMAND itermcolors_tests ${mode})
endforeach()

# DS-15：各条脱敏规则与收集流程分别登记。
add_executable(diag_bundle_tests tests/diag_bundle_tests.cpp)
target_link_libraries(diag_bundle_tests PRIVATE uicfg)
foreach(mode known fingerprint guid ipv4 ipv6 pipe home context logs summary bundle large filename save)
  add_test(NAME uicfg_diag_${mode} COMMAND diag_bundle_tests ${mode})
endforeach()

# DS-16：快捷键展示的状态、顺序与名称覆盖。
add_executable(keybinding_view_tests tests/keybinding_view_tests.cpp)
target_link_libraries(keybinding_view_tests PRIVATE uicfg)
foreach(mode defaults changed unbound invalid conflicts unknown coverage fixed)
  add_test(NAME uicfg_keybinding_${mode} COMMAND keybinding_view_tests ${mode})
endforeach()

# T08 Win32 主外壳（仅 WIN32 / mingw）。在仓库顶层构建时链接 it2w_pane_backend（目标在 app/pane_backend 定义，
# 生成阶段解析，无需顶层改动）；单独 cmake -S ui 时用占位后端，只验证外壳本身。
if(WIN32)
  execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    OUTPUT_VARIABLE IT2W_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE IT2W_GIT_RESULT)
  if(NOT IT2W_GIT_RESULT EQUAL 0 OR IT2W_BUILD_COMMIT STREQUAL "")
    set(IT2W_BUILD_COMMIT "unknown")
  endif()
  enable_language(RC)
  add_executable(it2w_shell WIN32 shell_win32/main.cpp shell_win32/connection_dialog.cpp shell_win32/diag_dialog.cpp shell_win32/keybinding_dialog.cpp shell_win32/clipboard_prompt.cpp shell_win32/it2w_shell.rc)
  target_compile_definitions(it2w_shell PRIVATE UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
  target_compile_definitions(it2w_shell PRIVATE IT2W_BUILD_COMMIT="${IT2W_BUILD_COMMIT}")
  if(IT2W_SSH_DIRECT AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    target_compile_definitions(it2w_shell PRIVATE IT2W_SSH_DIRECT=1)
  endif()
  target_link_libraries(it2w_shell PRIVATE uicfg comctl32 comdlg32 dwrite shell32 ole32 user32 gdi32)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    target_compile_definitions(it2w_shell PRIVATE IT2W_SHELL_STUB_BACKEND)
  else()
    target_link_libraries(it2w_shell PRIVATE it2w_pane_backend)
  endif()
  if(MINGW)
    target_compile_options(it2w_shell PRIVATE -Wall -Wextra -municode)
    target_link_options(it2w_shell PRIVATE -municode -static -static-libgcc -static-libstdc++)
  elseif(MSVC)
    target_compile_options(it2w_shell PRIVATE /utf-8)
  endif()
  # 所有 Windows 外壳构建均附带项目与第三方许可声明。
  add_custom_target(it2w_license_notices ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:it2w_shell>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE"
      "$<TARGET_FILE_DIR:it2w_shell>/licenses/termv2-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann_json/LICENSE"
      "$<TARGET_FILE_DIR:it2w_shell>/licenses/nlohmann_json-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/unicode/LICENSE.txt"
      "$<TARGET_FILE_DIR:it2w_shell>/licenses/Unicode-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD-PARTY-NOTICES.md"
      "$<TARGET_FILE_DIR:it2w_shell>/licenses/THIRD-PARTY-NOTICES.md"
    DEPENDS it2w_shell
    VERBATIM)
endif()
