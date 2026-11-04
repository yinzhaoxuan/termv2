# DS-9 最小外观 Profile：配色、回滚、内存

日期：2026-11-04
基线：main `2cc8b40`
出处：`REVIEW.md` GAP-09、§8 D3/D4；`REVIEW-增补-2026-10-31.md` §3 DS-9；`docs/TODO.md` §2.1；ROADMAP F-1（只取其中最小子集）。

凡写「决定」的地方，是本文替原设计补的选择。

## 0. 现状（2026-11-04 核实）

| 项 | 现状 | 问题 |
|---|---|---|
| 渲染配色 | `renderer/layout/cell_style.h` 的 `itren::Theme` 写死：前景 `D4D4D4`、背景 `1E1E1E`、VS Code 风格 16 色；光标画刷写死 `3399FF`（`renderer.cpp` 创建 `brush_caret` 处）。渲染器从不调用 `itcore_snapshot_colors`。 | ① OSC 4/10/11/12 改色在屏幕上**不生效**（核心改了，渲染器不看）。② OSC 10/11 查询按核心默认回答 `C7C7C7` / `000000`，屏幕实际是 `D4D4D4` / `1E1E1E`，属于不诚实应答（与 GAP-16 同类）。 |
| 核心配色 | `itcore_default_colors`：xterm 16 色、前景/光标 `C7C7C7`、背景 `000000`；`itcore_set_colors` 可下发配置色。 | 后端只在 `PaneBackendConfig.colors` 有值时下发，而 shell 从不设置它。 |
| 回滚 / 内存 | 核心默认 10000 行 / 64 MiB（`core.cpp`、`screen.cpp`，传 0 即默认）。`PaneBackendConfig` 写死 5000 / 32 MiB。 | 与设计不一致（GAP-09），用户不能改（D4）。 |
| Profile | `Connection.appearanceProfileId` 读写都在（缺省 `"appearance-default"`），没有任何地方使用。 | D3。 |

## 1. 目标与边界

做：

1. **颜色只有一个来源：核心的配置色。** 渲染器每帧从快照取生效色（配置色 + OSC 覆盖）。这样 OSC 改色可见，查询应答与屏幕一致。
2. **settings.json 里的外观 Profile**：前景、背景、光标、16 色、回滚行数、内存上限。连接按 `appearanceProfileId` 引用；本地默认 shell 用 `appearance-default`。
3. **默认值统一**：回滚 10000 行、内存 64 MiB，与核心相同；后端不再有自己的数。
4. **导入 `.itermcolors`**，写进 `appearance-default`；已打开的、使用该 Profile 的窗格立即换色。

不做（留给 F-1 / D1）：

- Profile 编辑界面、Profile 列表界面、给连接选 Profile 的界面（连接编辑器 D1 之前，只能手改 connections.json）。
- 字体按 Profile 设置（字体仍是 settings.json 顶层的全局值）。
- 选区色、粗体色、链接色、光标文字色、徽章色、深浅色双套配色、背景图与透明度。
- 256 色里 16–255 的自定义（与 iTerm2 一致，只配 16 色；16–255 用 xterm 标准值）。
- 改动已打开窗格的回滚 / 内存上限（只对新建窗格和重连生效，见 §5.3）。
- 会话 / 应用两级内存账本（E3）。本文的「内存上限」仍只是历史缓冲的上限，不是会话实际占用的上限。

约束（同 DS-2 §0）：逻辑放 `ui/src`（uicfg）、`app/pane_backend/pane_logic.*`、`renderer/layout/`，并在 macOS / Linux ctest 单测；`main.cpp`、`win32_pane_backend.cpp`、`renderer/d2d/renderer.cpp` 只做胶水。不改 `ITCORE_ABI_VERSION`，不改 `ports/core` 的行为（核心默认色保持 xterm，供无前端的测试与工具使用）。

## 2. 颜色数据流

```
settings.json appearanceProfiles ─┐
connections.json appearanceProfileId ─┤ uicfg::resolveAppearance
                                     ▼
                         uishell::PaneAppearance
                                     │  makeFactory 的 resolveAppearance 回调（每建一个窗格调一次）
                                     ▼
          Win32PaneBackend：toCoreColors() → itcore_set_colors（每个新核心，含重连）
                                     │
                                     ▼  OSC 4/10/11/12/104/110–112 在核心里覆盖 / 恢复
                       itcore_snapshot_colors(快照)
                                     │
                                     ▼  itren::themeFromColors()
                               D2D 渲染器 Theme
```

### 2.1 共享类型（`ui/include/uishell/pane_backend.h`）

```cpp
// 0xRRGGBB。只含 Profile 可配的部分；256 色的 16–255 由核心按 xterm 标准补齐。
struct PaneColors {
  std::uint32_t foreground, background, cursor;
  std::array<std::uint32_t, 16> ansi;
  bool operator==(const PaneColors&) const = default;
};
struct PaneAppearance {
  PaneColors colors;
  std::uint32_t scrollbackLines;
  std::uint64_t memoryLimitBytes;
};
// PaneBackend 新增（默认空实现，桩和测试替身不必改）：
//   替换本窗格核心的配置色；OSC 临时覆盖随之清掉（itcore_set_colors 语义）。之后的重连沿用新颜色。
virtual void setColors(const PaneColors& colors) { (void)colors; }
```

### 2.2 内置默认值（决定）

内置 `appearance-default` 的颜色 **取今天屏幕上实际看到的值**，升级后外观不变：

- 前景 `#D4D4D4`、背景 `#1E1E1E`、光标 `#3399FF`
- 16 色：`000000 CD3131 0DBC79 E5E510 2472C8 BC3FBC 11A8CD E5E5E5 666666 F14C4C 23D18B F5F543 3B8EEA D670D6 29B8DB FFFFFF`（即现 `itren::Theme::ansi`）
- 回滚 `10000` 行、内存 `64` MiB（= 核心默认）

这组值只在 uicfg 里定义一次（`uicfg::builtinAppearance()`）。核心的 xterm 默认色不动；后端总是调用 `itcore_set_colors`，所以 Windows 前端不再落到核心默认色上。

### 2.3 后端（`app/pane_backend`）

- `PaneBackendConfig` **删除** `scrollbackLines`、`memoryLimitBytes`、`colors` 三个字段，新增：

  ```cpp
  // connectionId → 外观（在 UI 线程、创建窗格时调用一次）。未设置：核心默认色与默认回滚 / 内存（传 0）。
  std::function<uishell::PaneAppearance(const std::string& connectionId)> resolveAppearance;
  ```

- 窗格构造时调用一次，把结果存为成员 `appearance_`。每次创建核心（首次启动与重连）：`opt.scrollback_lines` / `opt.memory_limit_bytes` 取 `appearance_`，随后 `itcore_set_colors(core_, toCoreColors(appearance_.colors))`，在首次 feed 之前。
- `setColors(c)`：在 UI 线程更新 `appearance_.colors`；有核心时立即 `itcore_set_colors` 并请求重绘。
- 纯函数 `it2app::toCoreColors(const uishell::PaneColors&) -> itcore_colors`（`pane_logic.*`）：先 `itcore_default_colors`，再覆盖前景、背景、光标与 `palette[0..15]`。单测。

### 2.4 渲染器（`renderer/`）

- `itren::Theme` 改为：`fg`、`bg`、`cursor`、`std::array<uint32_t, 256> palette`、`search`；加 `operator==`。默认构造值 = §2.2 的内置色 + xterm 标准 16–255（与核心 `itcore_default_colors` 的 16–255 相同），只在没有快照时用。
- `palette256(t, i)` 直接查 `t.palette[i]`。
- 新增纯函数 `itren::themeFromColors(const itcore_colors&, const SearchColors&) -> Theme`（`cell_style.h`）。
- `D2DRenderer` 每帧（有快照时）：`itcore_snapshot_colors` → `themeFromColors`；与上一帧的 `Theme` 不同则替换、整屏标脏（`I.dirty.invalidate()`，布局外区域也要按新背景清屏），并把 `brush_caret` 设为 `theme.cursor`。块光标上的字仍按该格解析出的背景色画，不变。

核心在改色时已推进全部行 revision（`colors_changed`），但布局外的边缘区域只在整屏重画时清，所以必须显式整屏标脏。

## 3. settings.json schema

新增可选顶层字段 `appearanceProfiles`：对象，键是 Profile ID。示例：

```json
"appearanceProfiles": {
  "appearance-default": {
    "foreground": "#D4D4D4",
    "background": "#1E1E1E",
    "cursor": "#3399FF",
    "ansi": ["#000000", "#CD3131", "…共 16 项…"],
    "scrollbackLines": 10000,
    "memoryLimitMiB": 64
  },
  "prod": { "background": "#2B0000" }
}
```

规则：

- **每个字段都可省略**，省略的字段取内置默认（§2.2）。上例的 `prod` 只改背景。
- 颜色写 `"#RRGGBB"`（大小写都收，保存时写大写）。`ansi` 必须正好 16 项。
- `scrollbackLines`：整数，100–1000000。`memoryLimitMiB`：整数，8–1024。两者谁先到谁生效（核心历史按行数和字节数两个上限淘汰）。
- Profile ID：非空，不含控制字符，≤ 64 字节。
- **未知键原样保留**：每个 Profile 解析时保存原始 JSON 对象；保存时在原对象上覆盖已知字段再写回。这样较新版本写的字段不会被旧版本删掉。
- **非法 Profile**（任一已知字段类型或范围不对）：该 Profile 整个按「不存在」处理（引用它的连接回退到内置默认），原值原样写回，启动时状态栏提示一次「外观 Profile「<id>」无效：<字段>，已用默认」（同 osc52 的做法）。其他 Profile 不受影响。
- `appearanceProfiles` 本身不是对象：整个字段按缺省处理，原值原样写回，提示一次。
- 文件里没有 `appearanceProfiles` 时，保存不新增这个字段（与 osc52 一样保持「不写就没有」）。导入配色时才会出现。
- `schemaVersion` 不变（仍为 1）：新增的是可选字段，旧版本读到会忽略。

uicfg 类型（`ui/include/uicfg/settings.h`，或新文件 `appearance.h`）：

```cpp
struct AppearanceProfile {
  std::optional<std::uint32_t> foreground, background, cursor;
  std::optional<std::array<std::uint32_t, 16>> ansi;
  std::optional<std::uint32_t> scrollbackLines;
  std::optional<std::uint32_t> memoryLimitMiB;
  std::string rawObject;          // 原始 JSON 对象（序列化文本）；新建时为 "{}"
  std::optional<std::string> invalidField;  // 有值 = 非法，内容是第一个出错的字段名
};
// SettingsFile 新增：
std::map<std::string, AppearanceProfile> appearanceProfiles;
std::optional<std::string> appearanceProfilesRaw;  // 字段存在但不是对象时的原值
```

## 4. 解析规则（`uicfg::resolveAppearance`）

```cpp
uishell::PaneAppearance builtinAppearance();
struct AppearanceResolution { uishell::PaneAppearance appearance; std::optional<std::string> warning; };
AppearanceResolution resolveAppearance(const SettingsFile&, const std::string& profileId);
// connectionId → profileId：空 connectionId 或找不到连接 → "appearance-default"；否则取连接的 appearanceProfileId（空串也按 "appearance-default"）。
std::string appearanceProfileIdFor(const ConnectionsFile&, const std::string& connectionId);
```

1. 从 `builtinAppearance()` 起步。
2. ID 不是 `appearance-default`，且 settings 里**没有**这个 ID → 回退到 `appearance-default` 的解析结果，`warning = "外观 Profile「<id>」不存在，已用默认"`。
3. ID 存在但非法 → 同上回退，`warning` 写明字段。
4. `appearance-default` 本身缺失 → 内置默认，无警告；非法 → 内置默认，有警告。
5. 命中有效 Profile：**先**套 `appearance-default`（若有效），**再**套该 Profile 的字段（逐字段覆盖）。这样 `prod` 只写背景时，其余颜色跟随导入到默认 Profile 的配色。
6. 内存 MiB → 字节在这里换算。

不阻止启动：Profile 问题只影响外观，不进 `validateInFile` 的阻断错误。警告由 shell 每个 Profile ID 只提示一次（状态栏 + ODS）。

## 5. 界面（`main.cpp`，薄胶水）

### 5.1 系统菜单

在「字体…」下面加两项：

- 「导入 iTerm2 配色…」：`GetOpenFileNameW`，过滤器 `*.itermcolors`。读文件（> 1 MiB 拒绝）→ `uicfg::importItermColors` → 确认框（文案由纯函数生成，见 §6.3）→ `uicfg::applyImportedColors(settings, "appearance-default", colors)` → 保存 settings.json → §5.2 实时换色。`applyImportedColors` 返回该 Profile 仍然非法的字段（如手写的 `scrollbackLines: 5`）；有值时状态栏说明「配色已写入，但默认外观的 <字段> 无效，未生效」，而不是报导入成功。
- 「恢复默认配色」：`uicfg::clearProfileColors(settings, "appearance-default")`（删掉该 Profile 的 5 个颜色字段；Profile 若因此只剩空对象且没有未知键，就删掉这个 Profile；`appearanceProfiles` 变空且原文件没有这个字段时整个删掉）→ 保存 → §5.2。

settings.json 只读（高版本或损坏）时：仍在内存里应用并换色，状态栏提示「settings.json 只读，配色未保存（本次运行有效）」，与字体一致。

### 5.2 已打开窗格实时换色

对每个窗格：用它的 connectionId 重新 `resolveAppearance`，`backend->setColors(结果.colors)`。只改颜色；回滚 / 内存不动。因为 §4 第 5 条的继承，改 `appearance-default` 会影响所有没有自己颜色字段的 Profile，所以直接对**全部**窗格重新解析，而不是只挑 ID 相同的。

### 5.3 回滚与内存的生效时机

新窗格、重连（重连沿用窗格构造时解析的值，见 §2.3）。手改 settings.json 后需要新开窗格。本文不提供「重新加载 settings.json」，与现有字体 / osc52 的做法一致。

## 6. `.itermcolors` 导入（`uicfg`，纯函数）

```cpp
struct ItermColorsImport {
  std::optional<uishell::PaneColors> colors;  // 失败为 nullopt
  std::string error;                          // 失败原因
  std::vector<std::string> missing;           // 缺的键（该项保留内置默认）
  std::vector<std::string> ignored;           // 认识但不支持的键（选区色等）
  bool approximated = false;                  // 有 Calibrated / 未知色彩空间按 sRGB 近似
};
ItermColorsImport importItermColors(std::string_view fileBytes);
```

### 6.1 文件格式

XML plist：`<plist><dict>` 顶层字典，键为颜色名，值为字典，含 `Red Component` / `Green Component` / `Blue Component`（`<real>` 或 `<integer>`，0–1）、可选 `Alpha Component`、可选 `Color Space`（`<string>`）。

只实现 plist 的这个子集，不引第三方 XML 库：

- 跳过 `<?xml …?>`、`<!DOCTYPE …>`、`<!-- … -->` 和标签间空白。
- 认识 `plist`、`dict`、`key`、`real`、`integer`、`string`、`true/`、`false/`、`array`（`array` 只需能跳过）。其他标签 → 失败。
- 文本里解码 `&amp; &lt; &gt; &quot; &apos;` 和 `&#N;` / `&#xN;`。
- 数字解析不受 C 库 locale 影响（不用裸 `strtod` / `atof`）。
- 以 `bplist00` 开头 → 失败，`error = "这是二进制 plist，请先转换：plutil -convert xml1 <文件>"`。
- 嵌套深度 > 8、文件 > 1 MiB → 失败。
- 一个颜色都没读到 → 失败（「不是 iTerm2 配色文件」）。

### 6.2 键映射

| `.itermcolors` 键 | 目标 |
|---|---|
| `Ansi 0 Color` … `Ansi 15 Color` | `ansi[0..15]` |
| `Foreground Color` | `foreground` |
| `Background Color` | `background` |
| `Cursor Color` | `cursor` |
| `Selection Color`、`Selected Text Color`、`Bold Color`、`Link Color`、`Cursor Text Color`、`Cursor Guide Color`、`Badge Color`、`Underline Color`、`Tab Color` | `ignored` |

- 每个目标键先找不带后缀的；没有就找 ` (Dark)` 后缀的（iTerm2 3.5 起的双套配色导出）；都没有就进 `missing`，该项用内置默认。` (Light)` 不用。
- 被忽略的键也按同样规则去后缀后只记一次。其他未知键静默忽略。
- 分量：钳到 [0,1]，`round(x * 255)`。Alpha 忽略。
- 色彩空间：缺省或 `sRGB` → 直接用；`P3` → Display P3 转 sRGB（去 sRGB 传递函数 → 线性 P3→sRGB 3×3 矩阵 → 钳到 [0,1] → 加回传递函数）；`Calibrated` 或其他 → 按 sRGB 近似，`approximated = true`。

### 6.3 确认框文案

纯函数 `uicfg::itermImportPrompt(const ItermColorsImport&, const std::string& fileName) -> std::string`：

```
导入「<fileName>」的配色到默认外观？
已读取：前景、背景、光标、16 色中的 <n> 项
缺少（保留默认）：<键1>、<键2>…            ← 有才显示
不支持（已忽略）：选区色、粗体色…           ← 有才显示，用中文名
部分颜色的色彩空间按 sRGB 近似。             ← approximated 时
```

`MB_OKCANCEL | MB_ICONQUESTION`。失败时 `MB_ICONWARNING` 显示 `error`。

## 7. 测试

平台无关，进 ctest：

- **uicfg settings**：`appearanceProfiles` 缺省 / 正确 / 部分字段 / 各字段非法（颜色格式、ansi 不足 16、范围外、类型错）/ 字段本身不是对象；未知键保存后仍在；非法 Profile 原值写回；没写这个字段时保存不新增；颜色大小写归一。
- **resolveAppearance**：内置默认；Profile 不存在 → 回退 + 警告；非法 → 回退 + 警告；继承（`appearance-default` 改了 ansi，`prod` 只改背景 → ansi 跟默认、背景用 prod）；MiB 换算；`appearanceProfileIdFor` 的空 ID / 找不到连接 / 空串。
- **importItermColors**：一个真实的 iTerm2 导出样本（入仓到 `ui/tests/data/`，选一个许可证允许的方案或自己写）；`(Dark)` 后缀回退；缺键；忽略键列表；`<integer>` 分量；P3 转换（纯红 P3 → sRGB 钳位后 `FF0000`，取一个中间色对照预先算好的值，允许 ±1）；Calibrated 置 `approximated`；二进制 plist；非法 XML；实体解码；超深嵌套；locale 无关（测试里 `setlocale(LC_NUMERIC, "de_DE.UTF-8")` 成功时再跑一遍，失败就跳过这一步）。
- **applyImportedColors / clearProfileColors**：新建 Profile、覆盖颜色不动回滚、清除后空 Profile 被删、有未知键时不删。
- **itermImportPrompt**：各行出现条件。
- **pane_logic**：`toCoreColors` 覆盖 0–15 与前景/背景/光标，16–255 等于 `itcore_default_colors`。
- **renderer**：`themeFromColors` 的映射；`palette256` 查表；默认 `Theme` 与 `uicfg::builtinAppearance()` 的颜色一致（如果 renderer 测试不链接 uicfg，就在 uicfg 测试里对照一份常量，二者都写注释指向对方）；现有 `test_cell_style`、`test_search_highlight` 适配新 `Theme`。

只能在 Windows 上验的（写进 STATUS「未验证」）：

1. 升级后外观与之前一致（默认 Profile）。
2. 导入一个 `.itermcolors`（如 Solarized Dark）后，已打开的本地窗格和 SSH 窗格立即换色；新开窗格也是新配色；恢复默认配色后回到原样。
3. `printf '\e]11;#336699\a'` 后背景变色（以前不变）；`printf '\e]11;?\a'` 的应答与屏幕背景一致；`printf '\e]111\a'` 恢复。
4. connections.json 给某连接写 `"appearanceProfileId": "prod"`、settings.json 定义 `prod` 只改背景，该连接背景不同、其余一致；写一个不存在的 ID，状态栏提示一次并用默认。
5. `scrollbackLines: 200` 的 Profile：输出 1000 行后历史只剩约 200 行；默认 Profile 能回滚到 10000 行。
6. CI `mingw` 编译通过。

## 8. 改动清单

| 文件 | 改动 |
|---|---|
| `ui/include/uishell/pane_backend.h` | `PaneColors`、`PaneAppearance`、`PaneBackend::setColors` |
| `ui/include/uicfg/settings.h`（或新 `appearance.h`）、`ui/src/settings.cpp`（或新 `appearance.cpp`） | schema 解析 / 保存、`builtinAppearance`、`resolveAppearance`、`appearanceProfileIdFor`、`applyImportedColors`、`clearProfileColors` |
| 新 `ui/include/uicfg/itermcolors.h`、`ui/src/itermcolors.cpp` | `importItermColors`、`itermImportPrompt` |
| `ui/CMakeLists.txt`、`ui/tests/*` | 新源文件与测试 |
| `app/pane_backend/win32_pane_backend.h/.cpp` | 删三个字段、加 `resolveAppearance`、`appearance_`、每个核心下发配色、`setColors` |
| `app/pane_backend/pane_logic.h/.cpp`、`tests/app/` | `toCoreColors` + 单测 |
| `renderer/layout/cell_style.h`、`renderer/d2d/renderer.cpp`、`tests/renderer/*` | 新 `Theme`、`themeFromColors`、每帧取快照配色、光标画刷 |
| `ui/shell_win32/main.cpp` | `makeFactory` 接 `resolveAppearance`；系统菜单两项；实时换色；Profile 警告去重 |
| 桩后端（`IT2W_SHELL_STUB_BACKEND`） | 无需改（`setColors` 有默认实现） |
| `docs/STATUS.md`、`docs/TODO.md`、`REVIEW.md` §8 D3/D4 | 回写 |
