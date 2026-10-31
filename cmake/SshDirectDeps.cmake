# Reproducible source inputs (upstream release archives, never host DLLs).
# libssh2 1.11.1: BSD-3-Clause (+ agent_win.c's retained BSD-2-Clause notices).
# OpenSSL 3.5.9: Apache-2.0. License files are copied alongside the probe.
include_guard(GLOBAL)

if(NOT MINGW OR NOT WIN32)
  message(FATAL_ERROR "ssh_a_probe requires MinGW Windows; for native logic checks use -DIT2W_SSH_A_PROBE_ONLY=ON -DIT2W_SSH_A_PROBE_SELFTEST=ON")
endif()

include(ExternalProject)
find_package(Perl REQUIRED)
find_program(SSH_A_MAKE NAMES gmake make REQUIRED)
include(ProcessorCount)
ProcessorCount(_ssh_a_jobs)
if(NOT _ssh_a_jobs)
  set(_ssh_a_jobs 2)
endif()
set(SSH_A_DEP_JOBS "${_ssh_a_jobs}" CACHE STRING "Parallel make jobs for the pinned OpenSSL")
set(_ssh_a_root "${CMAKE_BINARY_DIR}/ssh-a-deps")
set(_ssh_a_install "${_ssh_a_root}/install")
# Optional local archives for offline builds; URL_HASH is still enforced.
set(SSH_A_OPENSSL_URL "https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz" CACHE STRING "Pinned OpenSSL source archive URL/path")
set(SSH_A_LIBSSH2_URL "https://libssh2.org/download/libssh2-1.11.1.tar.gz" CACHE STRING "Pinned libssh2 source archive URL/path")
set(_ssh_a_ssl_hash "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a")
set(_ssh_a_ssh_hash "d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7")
get_filename_component(_ssh_a_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
get_filename_component(_ssh_a_cc_name "${CMAKE_C_COMPILER}" NAME)
if(NOT _ssh_a_cc_name STREQUAL "x86_64-w64-mingw32-gcc-posix")
  message(FATAL_ERROR "Use cmake/mingw-w64.cmake (x86_64-w64-mingw32-gcc-posix)")
endif()
set(_ssh_a_cross "${_ssh_a_cc_dir}/x86_64-w64-mingw32-")
# Configure writes CC=$(CROSS_COMPILE)gcc-posix; do not prepend the prefix twice.
ExternalProject_Add(ssh_a_openssl
  PREFIX "${_ssh_a_root}/openssl"
  URL "${SSH_A_OPENSSL_URL}" URL_HASH "SHA256=${_ssh_a_ssl_hash}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env CC=gcc-posix AR=ar RANLIB=ranlib WINDRES=windres
    ${PERL_EXECUTABLE} <SOURCE_DIR>/Configure mingw64
    --cross-compile-prefix=${_ssh_a_cross}
    --prefix=${_ssh_a_install} --libdir=lib --openssldir=${_ssh_a_install}/ssl
    no-shared no-module no-dso no-engine no-tests no-apps no-docs no-asm no-autoload-config
  BUILD_COMMAND ${SSH_A_MAKE} -j${SSH_A_DEP_JOBS} build_libs
  INSTALL_COMMAND ${SSH_A_MAKE} install_dev
  BUILD_BYPRODUCTS "${_ssh_a_install}/lib/libcrypto.a" "${_ssh_a_install}/lib/libssl.a")
ExternalProject_Add(ssh_a_libssh2
  PREFIX "${_ssh_a_root}/libssh2"
  DEPENDS ssh_a_openssl
  URL "${SSH_A_LIBSSH2_URL}" URL_HASH "SHA256=${_ssh_a_ssh_hash}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=<SOURCE_DIR>
    -P ${CMAKE_SOURCE_DIR}/cmake/ssh_a_openssh_only.cmake
  # Absolute OPENSSL_ROOT_DIR is re-rooted under MinGW FIND_ROOT_PATH; use the
  # dedicated toolchain (prefix first) and OPENSSL_ROOT_DIR=/.
  CMAKE_ARGS
    -DSSH_A_OPENSSL_PREFIX:PATH=${_ssh_a_install}
    -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_SOURCE_DIR}/cmake/mingw-w64-ssh-a-libssh2.cmake
    -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_INSTALL_PREFIX:PATH=${_ssh_a_install}
    -DCMAKE_INSTALL_LIBDIR:STRING=lib
    -DBUILD_SHARED_LIBS:BOOL=OFF -DBUILD_STATIC_LIBS:BOOL=ON
    -DBUILD_EXAMPLES:BOOL=OFF -DBUILD_TESTING:BOOL=OFF
    -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB:BOOL=ON
    -DENABLE_ZLIB_COMPRESSION:BOOL=OFF -DCRYPTO_BACKEND:STRING=OpenSSL
    -DOPENSSL_USE_STATIC_LIBS:BOOL=ON -DOPENSSL_ROOT_DIR:PATH=/
  BUILD_BYPRODUCTS "${_ssh_a_install}/lib/libssh2.a")
file(MAKE_DIRECTORY "${_ssh_a_install}/include")
add_library(it2w_dep_crypto STATIC IMPORTED GLOBAL)
set_target_properties(it2w_dep_crypto PROPERTIES
  IMPORTED_LOCATION "${_ssh_a_install}/lib/libcrypto.a"
  INTERFACE_INCLUDE_DIRECTORIES "${_ssh_a_install}/include"
  INTERFACE_COMPILE_DEFINITIONS LIBSSH2_STATIC)
add_library(it2w_dep_ssh2 STATIC IMPORTED GLOBAL)
set_target_properties(it2w_dep_ssh2 PROPERTIES
  IMPORTED_LOCATION "${_ssh_a_install}/lib/libssh2.a"
  INTERFACE_INCLUDE_DIRECTORIES "${_ssh_a_install}/include"
  INTERFACE_COMPILE_DEFINITIONS LIBSSH2_STATIC)
set_property(TARGET it2w_dep_crypto PROPERTY INTERFACE_LINK_LIBRARIES ws2_32 crypt32 bcrypt advapi32 user32)
set_property(TARGET it2w_dep_ssh2 PROPERTY INTERFACE_LINK_LIBRARIES it2w_dep_crypto)
add_dependencies(it2w_dep_ssh2 ssh_a_libssh2)
