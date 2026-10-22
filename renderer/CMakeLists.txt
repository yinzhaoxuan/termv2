# 渲染：平台无关的格度量与模拟快照（T06 原型与测试用，不与 itcore 同时链接）
add_library(itrender_layout INTERFACE)
target_include_directories(itrender_layout INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/layout)
add_library(itcore_mock STATIC mock/itcore_mock.cpp)
target_include_directories(itcore_mock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/mock ${PROJECT_SOURCE_DIR}/ports/core/include)
add_executable(test_renderer_mock_layout ${PROJECT_SOURCE_DIR}/tests/renderer/test_mock_layout.cpp)
target_link_libraries(test_renderer_mock_layout PRIVATE itcore_mock itrender_layout)
add_test(NAME renderer_mock_layout COMMAND test_renderer_mock_layout)
