// SPDX-License-Identifier: GPL-3.0-or-later
// DS-14：模态对话框只通过输入和提交回调接触配置。
#pragma once
#include <windows.h>
#include <functional>
#include <map>
#include <utility>
#include "uicfg/connection_form.h"

struct ConnectionDialogInput {
  std::wstring title;  // 「新建连接」/「编辑连接」。
  uicfg::ConnectionForm form;  // 初始表单；保存成功后更新为已提交的值。
  std::vector<std::pair<std::string, std::string>> groups;  // (id, 名称)，第一项为 ("", "未分组")。
  std::vector<std::string> appearanceProfiles;  // 原始 ID；显示后缀不参与保存。
  bool currentAppearanceUndefined = false;  // 当前外观 ID 未定义时显示提示后缀。
  bool canListVms = false;  // 占位后端为 false。
  std::map<std::string, int> fieldControls;  // 校验字段 → 控件 ID。
  // ADR-006：没写传输时按此认证方式此刻会不会走直连；未设置（占位后端）时下拉框只写「跟随全局默认」。
  std::function<std::optional<bool>(uicfg::AuthMode)> defaultTransportDirect;
  // 确定时调用：nullopt = 已保存并关闭对话框；否则显示错误并聚焦对应字段。
  std::function<std::optional<uicfg::ValidationError>(const uicfg::ConnectionForm&)> commit;
};
bool runConnectionDialog(HWND owner, HINSTANCE inst, ConnectionDialogInput&);
// 确定时调用 commit：返回错误文字，空 = 已保存并关闭对话框；runTextPrompt 返回 nullopt 表示取消。
std::optional<std::string> runTextPrompt(HWND owner, HINSTANCE inst, const std::wstring& title,
    const std::wstring& label, const std::string& initial,
    std::function<std::string(const std::string&)> commit);
