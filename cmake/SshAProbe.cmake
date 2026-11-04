# SPDX-License-Identifier: GPL-3.0-or-later
# Reproducible source inputs (upstream release archives, never host DLLs).
# libssh2 1.11.1: BSD-3-Clause (+ agent_win.c's retained BSD-2-Clause notices).
# OpenSSL 3.5.9: Apache-2.0. License files are copied alongside the probe.
if(IT2W_SSH_A_PROBE_SELFTEST)
  enable_testing()
  add_executable(ssh_a_probe_selftest platform/windows/probe/ssh_a_probe_selftest.cpp)
  if(NOT MSVC)
    target_compile_options(ssh_a_probe_selftest PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME SSH-A.logic COMMAND ssh_a_probe_selftest)
  if(MINGW)
    target_link_options(ssh_a_probe_selftest PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endif()
if(NOT IT2W_BUILD_SSH_A_PROBE AND (NOT IT2W_SSH_A_PROBE_ONLY OR IT2W_SSH_A_PROBE_SELFTEST))
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/SshDirectDeps.cmake")

add_executable(ssh_a_probe platform/windows/probe/ssh_a_probe.cpp)
target_compile_definitions(ssh_a_probe PRIVATE
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE NOMINMAX)
target_compile_options(ssh_a_probe PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ssh_a_probe PRIVATE it2w_dep_ssh2 gdi32)
target_link_options(ssh_a_probe PRIVATE -municode -static -static-libgcc -static-libstdc++)
include("${CMAKE_CURRENT_LIST_DIR}/SshDirectLicenses.cmake")
it2w_ssh_direct_notice_commands(_ssh_a_notice_commands
  "$<TARGET_FILE_DIR:ssh_a_probe>/ssh-a-licenses")
add_custom_command(TARGET ssh_a_probe POST_BUILD
  ${_ssh_a_notice_commands}
  VERBATIM)
