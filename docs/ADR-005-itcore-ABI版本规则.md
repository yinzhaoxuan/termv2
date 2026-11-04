# ADR-005：itcore C ABI 的版本与兼容规则

- **状态**：Accepted（2026-11-04）
- **出处**：`REVIEW-增补-2026-10-31.md` §3 DS-8（「`ITCORE_ABI_VERSION` 目前是 0，借这次改动定下 ABI 版本递增规则」）；`docs/TODO.md` §2.2「DS-8 余项」。
- **范围**：`ports/core/include/itcore.h` 公开的类型、常量、函数与它们的语义。

## 背景

- `itcore` 目前只作为仓库内的静态库被 `app/pane_backend`、`renderer`、测试使用，没有独立分发的 DLL，也没有仓库外调用方。
- `ITCORE_ABI_VERSION` 定义为 `0u`。`itcore_create` 要求 `itcore_options.abi_version` 与之**相等**；`itcore_buffer` 回填它。
- 调用方传入的结构体带 `struct_size`，核心目前一律按「`struct_size < sizeof(当前结构体)` 就拒绝」检查。
- 已经发生过的变化：SGR 扩展位（DS-8 / GAP-14）、新事件类型、新函数（搜索、颜色、剪贴板、OSC 52 读）、事件生存期的说明（DS-10），都没有动版本号，也没有书面依据说明为什么不用动。

## 决策

### 1. 版本号的含义

`ITCORE_ABI_VERSION` 是一个只增不减的整数，**只在不兼容变化时加 1**。规则从现在起生效，当前值 0 保持不变；下一次不兼容变化时改为 1。

不兼容（必须加 1）：

- 删除或重命名公开函数、类型、常量、枚举值。
- 改变函数签名（参数、返回类型、调用约定）。
- 改变已有结构体中**已有字段**的类型、顺序、偏移，或在中间插入字段。
- 改变已有枚举值或已用标志位的数值。
- 改变已有字段、标志位、返回值的**含义**，使旧调用方的理解出错（例如颜色编码 `0xTTRRGGBB` 的 TT 含义、坐标基准、所有权归属）。
- 收紧前置条件或生存期，让以前合法的调用变得非法。

兼容（不加 1）：

- 新增函数、类型、常量。
- 在枚举末尾追加新值（调用方必须忽略不认识的事件类型等，见第 3 条）。
- 启用 `itcore.h` 里标为「保留」的标志位（调用方必须忽略不认识的位）。
- 在**调用方填写**的结构体末尾追加字段，并按第 2 条处理尺寸。
- 放宽前置条件或生存期（例如 DS-10 写明「事件可晚于之后的 feed 释放」）。
- 修正缺陷，使行为符合头文件里已写明的契约。

### 2. `struct_size` 与末尾追加

- 调用方填写的结构体（`itcore_options`、`itcore_colors`、`itcore_clipboard_policy`、`itcore_key_event`、`itcore_mouse_event`、`itcore_search_options` 等）：在末尾追加字段时，同时在头文件里定义该结构体**最初版本**的尺寸常量（如 `ITCORE_SEARCH_OPTIONS_SIZE_V0`），核心改为「`struct_size < 最初尺寸` 才拒绝」，缺少的末尾字段按默认值处理。在第一次追加之前，现行的「`< sizeof` 拒绝」等价于此，不必先改。
- 核心填写、调用方提供缓冲的结构体（`itcore_search_progress` 等）：核心只写 `min(struct_size, sizeof)` 字节。追加字段时同样定义最初尺寸常量。
- `itcore_cell`、快照里的行数组这类**由核心分配、按数组访问**的结构体不能追加字段（调用方按 `sizeof` 步进）。要扩展只能新增查询函数，或者按第 1 条算不兼容。

### 3. 调用方的义务（写进 `itcore.h` 开头）

- 忽略不认识的事件类型（`itcore_next_event` 取出后照常 `itcore_event_release`）。
- 忽略 `itcore_cell.flags` 里不认识的位。
- 填写结构体时总是把 `struct_size` 设为 `sizeof(该结构体)`。

### 4. 机械检查

新增 `ABI-02.layout`（`tests/core/test_abi_layout.cpp`）：对公开结构体的 `sizeof` / `offsetof`、所有枚举值与已用标志位的数值做 `static_assert`，并断言 `ITCORE_ABI_VERSION` 等于表里记录的值。任何布局或数值变化都会让这项测试编译失败。改动者必须同时：

1. 按第 1 条判断兼容性；不兼容就把 `ITCORE_ABI_VERSION` 加 1；
2. 更新断言表；
3. 在提交信息里写「ABI：兼容 / 不兼容（版本 N→N+1）」。

断言只在 64 位目标上编译（指针和 `size_t` 为 8 字节；CI 的 Linux / macOS / MinGW x64 都满足），32 位目标跳过。

## 后果

- 当前没有仓库外调用方，所以规则的直接作用是让每次改动都留下明确的兼容性判断。等以后 itcore 以 DLL 或独立库形式提供给别的程序时，调用方可以靠版本号与 `struct_size` 判断能否使用。
- `itcore_create` 继续要求版本号**相等**。以后如果需要让新核心接受旧版本号，另写 ADR。
- 已经发生的变化（SGR 扩展位、新事件、新函数、DS-10 的生存期说明）按第 1 条都属于兼容变化，版本保持 0 是对的。
