# SPDX-License-Identifier: GPL-3.0-or-later
get_filename_component(GOLDEN_NAME "${GOLDEN}" NAME_WE)
# Token JSON is UTF-8 on every platform; Windows' active code page must not
# reinterpret it. Only normalize the stream's line endings (token controls are escaped).
execute_process(COMMAND "${EXE}" "${VEC}" OUTPUT_VARIABLE out RESULT_VARIABLE rc ENCODING UTF-8)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "itcore_test_tokens 退出码 ${rc}")
endif()
file(READ "${GOLDEN}" expected)
string(REPLACE "\r\n" "\n" out "${out}")
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT out STREQUAL expected)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${GOLDEN_NAME}.actual" "${out}")
  message(FATAL_ERROR "输出与黄金文件不一致，实际输出写到 ${GOLDEN_NAME}.actual")
endif()
