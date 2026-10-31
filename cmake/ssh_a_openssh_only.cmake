# libssh2 1.11.1 tries Pageant BEFORE OpenSSH, even when identity_path is set.
# Remove only that backend-table entry so this probe cannot accidentally pass
# with Pageant. The OpenSSH synchronous I/O/auth implementation is unmodified.
# Keep the pinned archive hash and all upstream license notices intact.
set(_agent "${SOURCE_DIR}/src/agent.c")
file(READ "${_agent}" _source)
set(_entry "    {\"Pageant\", &agent_ops_pageant},")
set(_replacement "    /* ssh_a_probe: Windows OpenSSH pipe only; no Pageant fallback. */")
string(FIND "${_source}" "${_entry}" _found)
if(_found EQUAL -1)
  string(FIND "${_source}" "${_replacement}" _patched)
  if(_patched EQUAL -1)
    message(FATAL_ERROR "Unexpected libssh2 agent.c; refuse to patch")
  endif()
else()
  string(REPLACE "${_entry}" "${_replacement}" _source "${_source}")
  file(WRITE "${_agent}" "${_source}")
endif()
