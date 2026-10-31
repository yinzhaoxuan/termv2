# Toolchain for the ssh_a libssh2 ExternalProject.
# MinGW re-roots absolute OPENSSL_ROOT_DIR under CMAKE_FIND_ROOT_PATH; put the
# pinned OpenSSL prefix first and pass -DOPENSSL_ROOT_DIR=/.
# try_compile re-reads the toolchain without -D cache vars unless listed here.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SSH_A_OPENSSL_PREFIX)
if(NOT SSH_A_OPENSSL_PREFIX)
  message(FATAL_ERROR "SSH_A_OPENSSL_PREFIX must point at the pinned OpenSSL install prefix")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/mingw-w64.cmake")
list(INSERT CMAKE_FIND_ROOT_PATH 0 "${SSH_A_OPENSSL_PREFIX}")
