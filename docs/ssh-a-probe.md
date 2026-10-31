# Windows A 探针（独立 ssh_a_probe.exe）

只验收直连可行性，不接入 PaneBackend，不调用 ssh.exe/ConPTY。默认 `--test all`；
**六项全部 PASS 且退出码为 0** 才通过 Windows 门槛。逻辑自检、交叉编译成功不能替代真机 PASS。

## 构建

Linux 需要 CMake ≥3.24、GNU make、Perl、C/C++20 的 mingw-w64 POSIX 工具链
（如 Debian/Ubuntu 的 `gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix`）。
从仓库根目录一条命令：

```sh
cmake -S . -B build-mingw/ssh-a -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DIT2W_SSH_A_PROBE_ONLY=ON && cmake --build build-mingw/ssh-a --target ssh_a_probe --parallel 4
```

产物 `build-mingw/ssh-a/ssh_a_probe.exe`，随附 `ssh-a-licenses/`；分发时一并携带。
普通工程构建默认关闭探针；也可加 `-DIT2W_BUILD_SSH_A_PROBE=ON`。
ONLY 入口不配置 UI。依赖装在构建目录内，不查宿主系统的 libssh2/OpenSSL DLL。
`-static -static-libgcc -static-libstdc++` 链接；Windows 自带系统 DLL 仍是正常依赖。
交付前检查：

```sh
x86_64-w64-mingw32-objdump -p build-mingw/ssh-a/ssh_a_probe.exe | rg 'DLL Name'
# 不应出现 libssh2、libcrypto、libssl、libgcc、libstdc++、libwinpthread 的 DLL。
```

固定源码输入（SHA-256，构建时强制验证）：

| 依赖 | 上游包 | SHA-256 |
| --- | --- | --- |
| libssh2 1.11.1 | [libssh2-1.11.1.tar.gz](https://libssh2.org/download/libssh2-1.11.1.tar.gz) | `d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7` |
| OpenSSL 3.5.9 | [openssl-3.5.9.tar.gz](https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz) | `603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a` |

libssh2 带一个明确的本地补丁：移除 agent 后端表里的 Pageant 条目。
1.11.1 原本优先尝试 Pageant，即便设置了 identity path；不移除会产生错误的 Windows OpenSSH PASS。
补丁脚本 `cmake/ssh_a_openssh_only.cmake` 可重复执行，源码不符则失败；OpenSSH agent I/O 和认证实现未改。
OpenSSL 禁用共享库、动态模块、配置自动加载和汇编。未引入 libssh/LGPL。
离线构建可用 `-DSSH_A_OPENSSL_URL=/path/openssl-3.5.9.tar.gz`
和 `-DSSH_A_LIBSSH2_URL=/path/libssh2-1.11.1.tar.gz`，hash 校验仍有效。
`-DSSH_A_DEP_JOBS=4` 控制 OpenSSL 内部 make 并行数。

## Windows 运行

在 PowerShell 中，先确认 OpenSSH agent 已加载服务端授权的密钥，known_hosts 已预信任目标：

```powershell
.\ssh_a_probe.exe --host 192.168.77.10 --port 22 --user LAB_USER --known-hosts "$env:USERPROFILE\.ssh\known_hosts" --cols 80 --rows 24 --test all
# Starting 多次目标尺寸：第一条 stty 应为 40 120，pty-req 不应先发 80x24。
.\ssh_a_probe.exe --host HOST --user USER --known-hosts 'C:\path\known_hosts' --starting-size 100x30 --starting-size 120x40 --test stty
# 未加密私钥只用于单项诊断，不能替代 all 的 agent 门槛。
.\ssh_a_probe.exe --host HOST --user USER --known-hosts 'C:\path\known_hosts' --identity 'C:\path\private_key' --test bytes
```

真机环境：客户端 **gavin**（2026-11-01 `--test all` PASS）与 DESKTOP-HOODQAJ / OpenSSH_for_Windows 9.5p2；服务端 UbuntuDev
`192.168.77.10:22`，账号全部由 CLI 指定。源码无 lab 主机/账号默认值。
`--host` 使用主机名或不带方括号的数字地址。路径支持 Unicode；不读 ssh_config、默认身份或 ASKPASS。
known_hosts **只读**，按 host（22）或 `[host]:port` 精确匹配，支持库可解析的普通/哈希条目；
未知/变化打印 `HostKeyUnknown`/`HostKeyChanged` 和 SHA256 指纹后失败。
libssh2 可能协商 **ECDSA** 主机密钥（与 OpenSSH `ssh.exe` 偏好的 ed25519 不同），known_hosts 须包含实际出示的密钥类型；失败时打印指纹便于核对。
解析失败或 `@` 标记明确失败；无 accept-new。
`--auth interactive`、密码、keyboard-interactive、加密私钥均非交互报错退出。
默认超时 `--timeout-ms 15000`（单项操作）；Ctrl+C 请求取消，也会使进程失败。

## 判据与测量边界

| 子测试 | 必须观察到的结果 |
| --- | --- |
| auth | 默认明确连接 `\\.\pipe\openssh-ssh-agent` 并通过公钥认证 |
| stty | 同一 channel 先 pty-req（默认 80×24，钳到 ≥20×5）再 shell；第一条 `stty size` 与初始目标相同 |
| bytes | 进入远端 raw/-echo/-opost 后运行提供的 R18-1 精确 printf，提取长度恰好为 79 的载荷逐字节比较 |
| resize | 同一 channel 连发 33 次 window-change，最后一次 137×43；随后 `stty size` 为 `43 137` |
| close-flood | 同一 channel `exec yes`，收到至少 1 MiB，再停应用读取 150 ms；关闭至 owner 线程退出 ≤3000 ms |
| close-auth | 独立认证 attempt：真实 agent 公钥经本地中继返回；sshd 接受公钥、发出 SIGN_REQUEST 后扣住签名应答；实际命中 CancelSynchronousIo，owner 和中继均退出且总计 ≤3000 ms |

`all` 的 auth/stty/bytes/resize/close-flood **只创建一次 session、一次 channel**。
close-auth 必须另开尚未完成认证的 attempt，不可能放在已经认证并开启 PTY 的 channel 上。
没有接受的 agent 身份、没到签名阶段、取消没命中、线程未收回都会 FAIL，不能用跳过冒充 PASS。
中继一直保持管道开启至 owner 清理结束，不能靠主动断开中继伪装取消成功。
窗口变更 want_reply=false；发送成功仅代表本地进度，最终效果由远端 stty 验证。
Starting 的 CLI 尺寸序列在工作线程中按 generation 合并，EAGAIN 请求参数固定。

关闭总预算从请求取消起计：EOF → channel close → disconnect，协议协商最多 2700 ms，
然后硬断 socket；仍由唯一 owner 释放库对象，线程退出也计入 3000 ms。
输出报告 `socket-hard-cut` 和耗时；预算内硬断符合探针关闭要求。
依赖卡死则 watchdog 打印 FAIL 后终止**整个探针**，不遗留认证线程，不当作 PASS。
远端 exit-status 原值打印；255 没有 ssh.exe 客户端错误语义。

风险：close-auth 覆盖的是未修改的 libssh2 Windows agent **签名等待处的同步 ReadFile 取消**，
真实 agent 的正常签名由 auth 覆盖；不声称覆盖硬件令牌确认、所有 busy/connect 时序或真实 agent 内部操作取消。
DNS 是工作线程的系统解析调用，只有场景 watchdog，未实现产品级可取消 DNS。
远端须为 POSIX 交互 shell，并提供 `stty`、`printf`、`yes`；自定义启动脚本可能干扰探针。
79 字节通过仅证明传输透明性，不代表 OSC52 策略/应答往返、Sixel 渲染或 tmux 控制模式交付。
固定依赖用于 lab 验证，产品化前仍需审查上游安全更新和算法支持。

## 无 Windows 的本地自检

```sh
cmake -S . -B /tmp/termv2-ssh-a-selftest -DIT2W_SSH_A_PROBE_ONLY=ON -DIT2W_SSH_A_PROBE_SELFTEST=ON
cmake --build /tmp/termv2-ssh-a-selftest --parallel 2
ctest --test-dir /tmp/termv2-ssh-a-selftest --output-on-failure
```

只测尺寸/generation、帧切分、逐字节损坏/截断、R18 printf 与独立数组一致、数值校验。
**不运行、不替代任何 Windows agent/网络验收项。**
