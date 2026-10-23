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

# 颜色解析（16/256/真彩、INVERSE）与按行脏区（DESIGN-T06 §3）
add_executable(test_renderer_cell_style ${PROJECT_SOURCE_DIR}/tests/renderer/test_cell_style.cpp)
target_link_libraries(test_renderer_cell_style PRIVATE itcore_mock itrender_layout)
add_test(NAME renderer_cell_style COMMAND test_renderer_cell_style)

# 渲染评审第 3/5 点：文本样式选择与 generation 不一致时的布局保持
add_executable(test_renderer_frame_policy ${PROJECT_SOURCE_DIR}/tests/renderer/test_frame_policy.cpp)
target_include_directories(test_renderer_frame_policy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/d2d ${PROJECT_SOURCE_DIR}/ports/core/include)
add_test(NAME renderer_frame_policy COMMAND test_renderer_frame_policy)

# DECSCUSR 光标几何、闪烁与 OSC 8 链接悬停下划线
add_executable(test_renderer_cursor_geometry ${PROJECT_SOURCE_DIR}/tests/renderer/test_cursor_geometry.cpp)
target_link_libraries(test_renderer_cursor_geometry PRIVATE itcore_mock itrender_layout)
add_test(NAME renderer_cursor_geometry COMMAND test_renderer_cursor_geometry)

# 字形缓存（LRU + shelf 图集）与字体回退规划
add_executable(test_renderer_glyph_cache ${PROJECT_SOURCE_DIR}/tests/renderer/test_glyph_cache.cpp)
target_link_libraries(test_renderer_glyph_cache PRIVATE itrender_layout)
add_test(NAME renderer_glyph_cache COMMAND test_renderer_glyph_cache)
