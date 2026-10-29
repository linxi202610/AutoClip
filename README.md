# AutoClip

选中即复制 / 双击粘贴 —— 一个 36 KB 的 Windows 小工具。

Select text or a picture, wait 200 ms, and AutoClip presses `Ctrl+C` for you.
Double-click where you want to type and it presses `Ctrl+V`. Both features can be
switched on and off independently.

![License](https://img.shields.io/badge/license-MIT-blue)
![Platform](https://img.shields.io/badge/platform-Windows%207%2B-lightgrey)
![Size](https://img.shields.io/badge/size-36%20KB-green)

---

## 功能 / Features

| | 说明 |
|---|---|
| **选中即复制** | 拖选文字、图片、表格单元格后松开鼠标，静置 200 ms（可调 120/200/300/500/800 ms），自动发送 `Ctrl+C`。双击选中一个词同样会复制。 |
| **双击粘贴** | 在 Word、记事本、浏览器输入框、搜索框等任意可输入位置双击左键，自动发送 `Ctrl+V`。 |
| **选项开关** | 两个功能各有独立开关；另有「智能模式」、复制延迟、开机自启、全局暂停。设置面板 + 托盘菜单 + 热键三处入口。 |
| **不污染剪贴板** | 复制永远由原程序完成（`Ctrl+C`），所以 RTF / HTML / PNG / 文件列表等多格式内容都不会被破坏。 |
| **防误伤** | 桌面、图标视图、任务栏一律跳过，绝不会把文件双击成「重命名」状态。 |
| **免安装** | 单文件 exe，不依赖运行时，不联网，不采集任何内容。 |

## 快速开始 / Quick start

1. 直接下载 **[AutoClip.exe](https://github.com/linxi202610/AutoClip/releases/latest/download/AutoClip.exe)**
2. 双击运行 → 托盘出现图标
3. 左键点托盘图标打开设置面板；右键是完整菜单

> 中文详细说明：[AutoClip-readme-cn.txt](https://github.com/linxi202610/AutoClip/releases/latest/download/AutoClip-readme-cn.txt)

```
托盘图标颜色      绿色 = 两个功能都开
                 琥珀色 = 只开了一个
                 灰色   = 全关 / 已暂停

Ctrl + Alt + O   打开设置面板
Ctrl + Alt + P   全局暂停 / 恢复
```

## 关于「双击」的冲突

Windows 里双击的原义就是「选中这个词」，和「双击粘贴」天生冲突。默认开启的
**智能模式**同时保住两者：

```
双击落在编辑器文字上   → 当成选词：只复制，绝不粘贴
双击落在编辑器空白处   → 当成粘贴：先还原你原来的剪贴板，再粘贴
双击落在桌面/图标上    → 直接忽略，不复制也不粘贴
```

想让双击在任何地方都粘贴：右键托盘 → 取消勾选 **Smart mode**。

## 构建 / Build

本项目用 [TCC](https://bellard.org/tcc/)（仓库内已附带 `tools/tcc`）交叉编译，
无需 Visual Studio。

```bat
tools\tcc\tcc.exe -DUNICODE -D_UNICODE -w ^
  -Itools\tcc\include -Itools\tcc\include\winapi -Ltools\tcc\lib ^
  -lshell32 -luser32 -lgdi32 -ladvapi32 ^
  -o release\AutoClip.exe src\autoclip2.c
```

> ⚠️ TCC 0.9.27 有个坑：**源码以换行结尾时，最后一个顶层定义会被当成宏指令吞掉**，
> 导致后面的函数全部 parse error。构建时请在文件末尾补一行 `/* eof */` 且不再加换行。

图标：`src/mkicon.c` 生成 7 种尺寸的 `AutoClip.ico`；`src/mkres.c` 在没有 `rc.exe` /
`windres` 的情况下，给 exe 追加一个 `IMAGE_RESOURCE_DIRECTORY` 资源段把图标真正嵌进去。

## 目录结构

```
src/autoclip.c      原始版本（保留作对比）
src/autoclip2.c     当前版本，主程序
src/logic_test.c    手势状态机 + 剪贴板判定单元测试（12 项）
src/e2e2.c          端到端测试（合成鼠标事件，需交互式桌面）
src/target2.c       测试用靶窗口
src/mkicon.c        .ico 生成器
src/mkres.c         PE 资源段嵌入器（替代 windres）
release/            发布产物：exe + ico + 使用说明
review.md           代码审查报告：原版本的 13 处问题与修复说明
```

## 已知限制

- 目标程序以管理员身份运行时，AutoClip 也需要以管理员身份运行。
- 全屏独占游戏、部分远程桌面窗口、UAC 弹窗不响应模拟按键。
- 智能模式靠窗口类名识别编辑器，极少数自绘编辑器（部分 IDE）识别不到时会退化为
  「直接粘贴」分支。
- 需要 `msvcrt.dll`（Windows 自带）。

## License

MIT，见 [LICENSE](LICENSE)。
