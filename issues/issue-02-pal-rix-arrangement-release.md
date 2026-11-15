# 本地 #2：PAL RIX 全通道编辑与发布

总管：本地 #1。GitHub：[othercat/AdVisual#1](https://github.com/othercat/AdVisual/issues/1)。

用户授权：更新 README 展示图，提交并推送代码，普通 merge 到 main，发布 Windows x64 和 Apple Silicon/macOS 15+ 下载包。只修改 othercat fork，不向 upstream 提交。

基线：main @ 757de6024455ad4e1c843c7daec9d8b341926438。
功能分支：richardli/feat/2-pal-rix-arrangement；合并目标 main 已由用户明确确认。
版本：v1.1.0。

本地已通过：86 首 RIX 的 ROL+BNK 保存重载、1008 个音色字段、旋律/打击乐三轨编辑/撤销/保存、实际 GTK 渲染及窗口播放。原始 RIX/MKF 不提交、不随包发布。

发布验收：Windows 实际 CI 构建及可运行包、macOS 可移动 .app 与依赖闭包、Release 源码和校验文件。旧 AdVisual-local.app 依赖开发机环境，不作为公开发行物。

实现提交：dc95a6fff693dade1ef102a31388299ee6260c91。
合并提交：main @ 958723e3579451b6077403923c47a8c8dcfc722b（普通merge；初次合并ab79bc3）。
Release：https://github.com/othercat/AdVisual/releases/tag/v1.1.0（已正式发布，公开下载）。
实机/听感验收：等待用户跨设备确认；Issue 保持开放。

2026-11-16 发布准备验证：macOS package-portable/check-portable 通过；递归依赖闭包、ad-hoc 签名、带空格的新路径、清理 Homebrew/GTK 环境后的 28 PNG/SVG loader/schema/系统字体与真实程序初始化均通过。主机已用打包后的 .app 打开 MUSIC_36.RIX，实际确认原生文件选择器与 11 通道全局界面。Windows CI 构建与包运行待远端验证。

## 跨平台发布修复记录

实现：dc95a6fff693dade1ef102a31388299ee6260c91。文档更新跳过重建：1116e01c33631d5a94e10d5a5b6fed3c253dcb5d。
- CI 43735495000：macOS 通过；Windows Python CRLF 导致 Git revision 尾部 CR，85c4b89d883b3d27e8577cdea8b62f2bf2d90ea8 修复，LF/CRLF shell 回归通过。
- CI 43736285104：Windows 两个音频依赖编译成功，windres 被 GTK CPU 标志打断；ea0967a3a917f2f94036b3f371ead562df403e9a 限制 flags 为 C/CXX，生成器 RC_FLAGS 为空，本机目标重建通过。
- CI 43737110976：Windows 全部编辑器与依赖编译/链接成功，打包误判系统 WINSPOOL.DRV；8210e9a5405a51c998e96c93c89cfc27806f4112 按 System32 实际文件识别，不放宽第三方依赖检查。
- be22ad5ca1748b5fcd7c2eddb594cbd18e5f37eb：无 .git 源码包读取 SOURCE-COMMIT，真正解包源码的 GTK 资源打包通过。
当前验证：https://github.com/othercat/AdVisual/actions/runs/43737773439。Windows 包与最终 main 构建验收中；发布后补全 merge SHA、Release 与服务端校验。
本机证据：process-root/artifacts/AdVisual-issue-2；GitHub 记录见 Issue #1。

CI 43737773439：Windows ZIP 与严格依赖闭包已成功生成，搬移检查的 plain dict 丢失 Windows 环境变量大小写查找语义，SystemRoot KeyError。已改为从原始 os.environ 取系统目录；实际搬移检查函数在模拟 Windows 大写键、可执行 probe fixture 下通过，同时复现旧 dict 失败。最终 main CI 将运行真实 Windows 便携包。

## main 发布启动修复

普通 merge：ab79bc3e5b6a596f4792b0b34f15a9ee9c2e30bb。CI 43738787645 的 macOS 包通过，Windows 严格闭包、ZIP 和实际 GTK 搬移 probe（28PNG/SVG/schema/font）通过；真实 advisual.exe 启动遇到32位 MinGW pseudo relocation越界。创建 main 基线修复分支 richardli/fix/2-windows-dll-relocations；MinGW x64 的 C/CXX 与链接启用 large code model，保留 ASLR/DEP。生成器验证x64 C/CXX/link均带flag、RC/x86无；Mac配置及真实目标构建通过。机制符合 GCC/ld 官方文档，但不把推断的具体外部符号当作已定位事实。最终实际Windows启动由后续main CI验证，新增失败二进制/PE/符号/构建命令诊断附件。证据：process-root/artifacts/AdVisual-issue-2/large-model。

CI 43740119139：large-model确实应用，但 GCC16 的 ArrangementView.cpp 产生未定义 _GLOBAL_OFFSET_TABLE_，候选未通过、未发布。修复分支 richardli/fix/2-windows-opl-data-import 改用局部方案：移除large-model 6行，VisPlayer两处 CPlayer::op_table 改为本地constexpr硬件映射。与固定AdPlug commit 3d9457ac91121302a752577b2bf063b4aea202b8 的 src/player.cpp 逐项一致：[0,1,2,8,9,10,16,17,18]。其它打击乐/音符/频率表已有本地static定义；binio只用枚举/类型。本机真实构建通过，nm显示 CPlayer::op_table 未定义导入已消失，剩余 CPlayer RTTI 不作为已定位故障符号陈述。最终Windows实际启动由后续main CI验证。失败诊断保留于 process-root/staging/AdVisual-issue-2/github-43740119139/diagnostics。

二进制定位（CI 43741327642）：53个32位pseudo records均为依赖函数；唯一匹配错误地址低16位 AF45 的项target RVA=0xaf45、IAT RVA=0x18bad8，对应 CNemuopl::CNemuopl(int)，不是op_table。另有177个64位数据项。完整PE/符号与解析脚本保存在 process-root/staging/AdVisual-issue-2/github-43741327642/diagnostics 和 inspect-pe.py。固定AdPlug/libbinio顶层CMake会在BUILD_SHARED_LIBS且IPO变量未定义时自动开启IPO；显式OFF后没有后续目标覆盖。修复分支 richardli/fix/2-windows-dependency-exports：两依赖configure均显式IPO=OFF，CI检查真实命令无-flto、构造函数和readInt导入库均有T类型函数thunk；随后严格便携与真实启动检查。候选仍以实际WindowsCI为准，不把机制推断提前写作通过。

CI 43742749690（main 30aac3420f3f453c47edc8e8f2d5ea87a9ff879a）双平台全部通过：两依赖无LTO、函数thunk类型、严格依赖闭包、GTK搬移probe、真实编辑器启动均通过。发行来源核对发现Windows原始Git状态为yes，可能由原生/MSYS Git的CRLF判定差异造成；包装元数据继续保留原始状态，另用真实 git diff --ignore-cr-at-eol HEAD 记录是否存在超出CRLF的源文件变化，不隐藏实质变更。本机从真实源快照创建Git fixture，原始状态yes/仅CRLF时新增字段no，追加实际内容后新增字段yes，资源打包均执行通过。后续main构建将验证最终源标识与实际包一致。

## 最终交付（2026-11-16）

版本 v1.1.0 tag 指向最终 main 合并节点 958723e3579451b6077403923c47a8c8dcfc722b。最终 CI https://github.com/othercat/AdVisual/actions/runs/43744086521 双平台全部通过：函数thunk/非LTO验证、依赖闭包、PNG/SVG/schema/font、带空格路径搬移和真实程序启动均通过。最终EXE解析：危险32bit函数pseudo relocation由53降为0，保留177项合法64bit。Win/Mac包与源码标识同提交；真实diff确认无超出CRLF的源文件改动，原始Git状态仍保留。
Release已公开，3个ZIP+SHA256SUMS.txt均已核对GitHub服务端digest：
- Windows x64：956ac077624c69626496b7b487d218f140a2cea4abe1e895ee9d365ae3793c24
- macOS arm64：47939ff38ce39a46bf24f4876aefa703e7d79669716ccc45cac0695bb1569ccb
- Source：25f751d9fc4f4442b73097dc2de35bcb2455272a5a144238d994c164a66e3f1c
macOS要求Apple Silicon / 15+，已附依赖和ad-hoc签名，未做Developer ID/公证。Windows无需开发环境。无游戏RIX/MKF/WAV/MP3随包。
源码与二进制副本、校验与服务端回执：delivery-root/artifacts/AdVisual-issue-2/github-v1.1.0。CI日志、PE验证与清理回执：process-root/artifacts/AdVisual-issue-2。所有5个本地开发/修复分支均通过远端main祖先检查，未合入rev-list为空；本地全部正常-d清理、唯一远端功能分支已删除，无worktree。原始提交均由main/tag恢复，未重写历史。
听感与其他设备验收：待用户确认，GitHub #1保持开放；Windows CI验证进程启动和资源搬移，设备预听/交互仍需用户实机验收。
