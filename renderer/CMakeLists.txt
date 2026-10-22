# 渲染：平台无关的格度量与模拟快照（T06 原型与测试用，不与 itcore 同时链接）
add_library(itrender_layout INTERFACE)
target_include_directories(itrender_layout INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/layout)
add_library(itcore_mock STATIC mock/itcore_mock.cpp)
target_include_directories(itcore_mock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/mock ${PROJECT_SOURCE_DIR}/ports/core/include)
add_executable(test_renderer_mock_layout ${PROJECT_SOURCE_DIR}/tests/renderer/test_mock_layout.cpp)
target_link_libraries(test_renderer_mock_layout PRIVATE itcore_mock itrender_layout)
add_test(NAME renderer_mock_layout COMMAND test_renderer_mock_layout)

# T06：Direct2D/DirectWrite 渲染原型（静态库不链 itcore，由可执行文件选择 mock 或真实核心）
add_library(itrender_d2d STATIC d2d/renderer.cpp)
target_include_directories(itrender_d2d PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/d2d
  ${CMAKE_CURRENT_SOURCE_DIR}/layout
  ${PROJECT_SOURCE_DIR}/ports/core/include)
if(WIN32)
  target_compile_definitions(itrender_d2d PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(itrender_d2d PUBLIC d2d1 dwrite d3d11 dxgi)
endif()

add_executable(test_renderer_d2d_caret ${PROJECT_SOURCE_DIR}/tests/renderer/test_d2d_caret.cpp)
target_link_libraries(test_renderer_d2d_caret PRIVATE itrender_d2d itrender_layout itcore_mock)
add_test(NAME renderer_d2d_caret COMMAND test_renderer_d2d_caret)
