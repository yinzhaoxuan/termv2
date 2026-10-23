# WinUI 3 探针 —— Windows 构建步骤（**未在云端构建/验证**）

前提：Windows 10 19041+ / Windows 11，Visual Studio 2022 17.8+，工作负载「使用 C++ 的桌面开发」+ 组件「Windows App SDK C++ 模板」（或「通用 Windows 平台开发」里的 C++ (v143) UWP 工具），Windows SDK 10.0.22621。

在「x64 Native Tools Command Prompt for VS 2022」里：

```bat
cd ui\experiments\winui3_probe
curl -L -o nuget.exe https://dist.nuget.org/win-x86-commandline/latest/nuget.exe
nuget.exe restore winui3_probe.vcxproj -PackagesDirectory packages
msbuild winui3_probe.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

产物：`x64\Release\winui3_probe\winui3_probe.exe`（自包含：同目录带 Microsoft.WindowsAppRuntime 等 DLL 与 resources.pri，整个目录即便携包）。
也可直接用 VS 打开 .vcxproj，平台选 x64 后 F5。

已知风险：
- NuGet 版本号（WindowsAppSDK 1.6.241114003 / CppWinRT 2.0.240405.15 / SDK.BuildTools 10.0.22621.3233 / WebView2 1.0.2651.64）写死；restore 报找不到时用 VS「管理 NuGet 包」升级到可用版本并同步改 .vcxproj 里的路径。
- 纯代码（无 App.xaml）需要 `DISABLE_XAML_GENERATED_MAIN` 与 App 实现 IXamlMetadataProvider（已做）；若 XamlControlsResources 报找不到类型，确认 provider 成员已构造。
- 若编译报错，记录首个错误即可 —— 「能否顺利搭起来」本身就是选型数据。
