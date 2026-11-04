// SPDX-License-Identifier: GPL-3.0-or-later
// DS-14：连接编辑器的可移植表单、分组与菜单规则。
#pragma once
#include "uicfg/store.h"

namespace uicfg {

enum class FormKind { SshManual, SshHyperV, Local };
struct ConnectionForm {
  std::string name, groupId, appearanceProfileId;
  FormKind kind = FormKind::SshManual;
  std::string host, port = "22", user;
  AuthMode auth = AuthMode::Agent;
  std::string identityFile;
  std::optional<SshTransport> transport;
  std::string connectTimeout = "15";
  std::string vmId, adapterId, fallbackHost;
  std::string executable, argumentsText, workingDirectory;
  bool argumentsEditable = true;
};

ConnectionForm formFromConnection(const Connection&);
ConnectionForm newConnectionForm(const std::string& groupId);
std::string newConnectionId(const ConnectionsFile&);
struct FormApplyResult {
  std::optional<Connection> connection;
  std::vector<ValidationError> errors;
};
// 只处理表单转换；成功后仍须 validateInFile，保留隐藏字段与未改动的 transport 原文。
FormApplyResult applyConnectionForm(const ConnectionForm&, const std::optional<Connection>& base,
                                   const ConnectionsFile&);

std::optional<std::string> groupNameError(const ConnectionsFile&, const std::string& name,
                                        const std::string& exceptGroupId = {});
// 名称非法时不修改；addGroup 返回空 ID，renameGroup 返回 false。
std::string addGroup(ConnectionsFile&, const std::string& name);
bool renameGroup(ConnectionsFile&, const std::string& groupId, const std::string& name);
int deleteGroup(ConnectionsFile&, const std::string& groupId);
std::string deleteConnectionPrompt(const Connection&, int runningSessions);
std::string deleteGroupPrompt(const Group&, int connectionCount);
// errorName 来自 hvQueryErrorName；不依赖平台头文件。
std::string hyperVListError(const std::string& errorName, const std::string& message);

enum class TreeTarget { Empty, LocalShell, Group, Ungrouped, Connection };
enum class TreeAction { Connect, NewConnection, EditConnection, DeleteConnection, NewGroup, RenameGroup, DeleteGroup };
struct TreeMenuItem { TreeAction action; std::string label; bool enabled; };
std::vector<TreeMenuItem> treeMenuFor(TreeTarget, bool readOnly);

}  // namespace uicfg
