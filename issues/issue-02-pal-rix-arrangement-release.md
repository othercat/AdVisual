# 本地 #2：PAL RIX 全通道编辑与发布

总管：本地 #1。GitHub：[othercat/AdVisual#1](https://github.com/othercat/AdVisual/issues/1)。

用户授权：更新 README 展示图，提交并推送代码，普通 merge 到 main，发布 Windows x64 和 Apple Silicon/macOS 15+ 下载包。只修改 othercat fork，不向 upstream 提交。

基线：main @ 757de6024455ad4e1c843c7daec9d8b341926438。
功能分支：richardli/feat/2-pal-rix-arrangement；合并目标 main 已由用户明确确认。
版本：v1.1.0。

本地已通过：86 首 RIX 的 ROL+BNK 保存重载、1008 个音色字段、旋律/打击乐三轨编辑/撤销/保存、实际 GTK 渲染及窗口播放。原始 RIX/MKF 不提交、不随包发布。

发布验收：Windows 实际 CI 构建及可运行包、macOS 可移动 .app 与依赖闭包、Release 源码和校验文件。旧 AdVisual-local.app 依赖开发机环境，不作为公开发行物。

实现提交：待记录。
合并提交：待记录。
Release：待记录。
实机/听感验收：等待用户跨设备确认；Issue 保持开放。

2026-11-16 发布准备验证：macOS package-portable/check-portable 通过；递归依赖闭包、ad-hoc 签名、带空格的新路径、清理 Homebrew/GTK 环境后的 28 PNG/SVG loader/schema/系统字体与真实程序初始化均通过。主机已用打包后的 .app 打开 MUSIC_36.RIX，实际确认原生文件选择器与 11 通道全局界面。Windows CI 构建与包运行待远端验证。
