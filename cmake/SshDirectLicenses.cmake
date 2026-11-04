# SPDX-License-Identifier: GPL-3.0-or-later
include_guard(GLOBAL)

# Call after SshDirectDeps has configured the pinned ExternalProjects.
# Both the probe and product copy upstream texts from those same source trees.
function(it2w_ssh_direct_notice_commands out_var destination)
  ExternalProject_Get_Property(ssh_a_libssh2 SOURCE_DIR)
  set(_libssh2_source "${SOURCE_DIR}")
  ExternalProject_Get_Property(ssh_a_openssl SOURCE_DIR)
  set(_openssl_source "${SOURCE_DIR}")
  set(${out_var}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${destination}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_libssh2_source}/COPYING"
      "${destination}/libssh2-COPYING.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_libssh2_source}/src/agent_win.c"
      "${destination}/libssh2-agent_win.c.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_openssl_source}/LICENSE.txt"
      "${destination}/OpenSSL-LICENSE.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../THIRD-PARTY-NOTICES.md"
      "${destination}/THIRD-PARTY-NOTICES.md"
    PARENT_SCOPE)
endfunction()
