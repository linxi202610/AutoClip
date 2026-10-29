# AutoClip 1.0 — 审查报告与改动说明

原代码：`src/autoclip.c`（875 行，TCC 编译，release/AutoClip.exe 26 KB）
新代码：`src/autoclip2.c`（约 1360 行，release/AutoClip.exe 36 KB）

结论：原程序能跑，功能主干是对的，但有几处会在真实使用中出问题，
其中「粘贴误触发」和「图片复制」是功能性缺陷，已重写修复。

---

## 一、原代码确实存在的问题

| # | 位置 | 问题 | 后果 |
|---|------|------|------|
| 1 | `OnActTimer` / `OnPasteTimer` | `SetTimer(TIMER_PASTE)` 在粘贴定时器回调还没结束时又被 `OnActTimer` 重新设置 | 定时器自嵌套，双击后可能连发多次 Ctrl+V |
| 2 | `OnPasteTimer` | `free(g_savedClip)` 之后没有把指针置空 | 悬挂指针，下一次 `OnActTimer` 再 `free` 一次会崩溃 |
| 3 | 全局 | 程序用 `ClipWriteText` 恢复了剪贴板；但很多控件（Word/浏览器/RichEdit）在无选区时按 Ctrl+C 会**清空或改写剪贴板**（写空格/空串），恢复逻辑只覆盖纯文本 | Word 里双击空白处 → 剪贴板被清空，用户原来复制的东西丢了 |
| 4 | 全局 | 双击粘贴没有任何「目标窗口」判断 | **Word 里双击选词会被 Ctrl+V 覆盖**；**桌面/资源管理器里双击文件名会直接进入重命名并粘贴乱码**（最严重） |
| 5 | `HasMeaningfulText` + 剪贴板比对 | 图片复制靠「剪贴板文本是否变化」判断，图片没有文本可比 | 双击选中图片时误判「没有选区」→ 乱粘贴 |
| 6 | 全文件 | 只装了鼠标钩子、没有 `RegisterHotKey`、没有暂停开关、没有设置界面 | 用户只能用托盘右键菜单，关闭/开启不方便（你明确要求「有选项自己选择」） |
| 7 | `RefreshTray` | 每次刷新都 `MakeIcon()` 造新图标，旧图标只在成功分支销毁；`NIM_ADD` 失败时直接 `return`，新 HICON 泄漏 | 长时间反复开关会泄漏 GDI 对象 |
| 8 | `RefreshTray` | 用 `NIM_MODIFY` 更改提示文字，但 `NOTIFYICON_VERSION_4` 从未设置 | 任务栏图标行为与新版 Windows 不一致 |
| 9 | `Arm`/`SetTimer` | 200 ms 等待用 `SetTimer`，但任何新的 `LBUTTONDOWN` 都 `KillTimer` | 双击的第二下会取消第一下排队的动作，双击路径依赖时序才偶然正确 |
| 10 | `MakeIcon` | `CreateIconIndirect` 前不检查 `hDC`，`hbmMask` 失败不检查；图标 32×32 固定 | 高 DPI 下模糊，极端情况返回坏图标 |
| 11 | 单例 | 第二次启动只弹框，不唤醒已有实例；`Local\` 前缀在会话间不隔离（恰好是对的） | 用户体验一般 |
| 12 | 自启 | `--debug` 与开机启动参数的路径拼接没做长度容错 | 路径极长时可能截断 |
| 13 | `e2e_result.txt` | 原测试只断言「剪贴板变了」「编辑框里有 ZZZ」，没有断言「双击选词不粘贴」「剪贴板不被污染」 | 最严重的第 4 条缺陷没被测出来 |

另外，`release/使用说明.txt` 里说的「智能模式：双击文字上 → 只复制不粘贴」在实现上其实只有
`DoDoubleGesture` 的文本框路径才大致成立，一旦双击发生在 Word/浏览器正文里就会直接粘贴。

---

## 二、新版本的对应改动

1. **手势状态机重写**：钩子里只做「记录 + 标记」，200 ms 后的判定放在消息线程里，
   新的按下只取消「尚未到期」的手势，双击的第二下不再误杀。
2. **不再伪造复制**：复制永远由 Ctrl+C 让原程序自己做，多格式（RTF/HTML/PNG/文件列表）不丢。
   唯一的写剪贴板只发生在「智能模式双击后确认没有选区」时，把操作前的纯文本原样写回。
3. **目标窗口分类**：`ClassifyTarget()` 判断 桌面 / 图标视图 / 编辑框 / 浏览器 / 本程序窗口：
   - 桌面、图标视图：**直接跳过，不复制也不粘贴**（彻底杜绝误改名）
   - 编辑框（Edit/RichEdit）：先 Ctrl+C 探测，有选区就只复制、不粘贴
   - 浏览器（Chrome_WidgetWin/Mozilla/IE_Server）：同样先探测
   - 其它：直接粘贴
4. **图片判定**：`HasRealPicture()` 检查 `CF_DIB / PNG / GIF / CF_BITMAP / CF_METAFILEPICT`，
   与序列号比对配合，双击选图不会被误判成「空白处」。
5. **设置界面**：真正的模态设置面板（`CreateWindowExW` + 复选框 + 延迟下拉框），
   热键 `Ctrl+Alt+O` 打开，`Ctrl+Alt+P` 全局暂停/恢复。托盘菜单同步所有开关。
6. **延迟可配**：120 / 200 / 300 / 500 / 800 ms，默认 200 ms。
7. **资源与线程安全**：图标按状态缓存（最多 3 个 HICON），定时器只有一个，
   `g_savedClip` 先置空再 `free`，无悬挂指针。
8. **任务栏兼容**：订阅 `TaskbarCreated`，资源管理器重启后图标自动回来；
   轮询间隔统一用系统 `SM_CXDOUBLECLK` / `GetDoubleClickTime()`，不再硬编码。
9. **高质量图标**：`src/mkicon.c` 生成 16/24/32/48/64/128/256 七种尺寸的 `AutoClip.ico`；
   `src/mkres.c` 在**没有 rc.exe/windres** 的情况下，给 exe 追加一个
   `IMAGE_RESOURCE_DIRECTORY` 资源段，把图标真的嵌进去
   （`ExtractAssociatedIcon` 已能取出 32×32 图标）。

---

## 三、测试

| 测试 | 文件 | 结果 |
|------|------|------|
| 手势状态机 + 剪贴板判定（12 项） | `src/logic_test.c` | **12/12 全部 PASS** |
| 图标生成 / 嵌入 | `src/mkicon.c`、`src/mkres.c` | 7 个尺寸写入，`ExtractAssociatedIcon` 取回成功 |
| 进程启动 / 钩子安装 / 消息循环 | `--debug` 日志 | `hook installed` → `entering message loop` |

**关于 e2e 的说明（重要）**：`src/e2e2.c` 那套「合成鼠标事件 → 看剪贴板」的
端到端测试在本会话里跑不通——本机的这个会话没有交互式桌面
（`GetForegroundWindow()` 恒为 0，`GetCursorPos` 返回 0，session 1 但无输入桌面），
`WH_MOUSE_LL` 钩子收不到 `SendInput` 产生的移动/点击事件。我单独写了
`src/hookdiag2.c` 验证过：钩子能装上，但连模拟的 300 次鼠标移动都收不到，
这是环境限制，不是程序缺陷。真实的鼠标/键盘操作在正常桌面会话下才会走到钩子。
因此端到端请在你自己桌面上用 Word/记事本手动验证一次（步骤见使用说明第八节）。

---

## 四、构建方法（可复现）

```
tools\tcc\tcc.exe -DUNICODE -D_UNICODE -w ^
  -Itools\tcc\include -Itools\tcc\include\winapi -Ltools\tcc\lib ^
  -lshell32 -luser32 -lgdi32 -ladvapi32 ^
  -o release\AutoClip.exe src\autoclip2.c   （末尾不要留空行，见下）
```

**踩到并已绕开的 TCC 0.9.27 编译陷阱**（同一个坑把原代码坑过一次）：

1. `advapi32`/`shell32` 的 `.def` 在 TCC 附带的 lib 里缺失，
   已在 `tools\tcc\lib\` 补了 `advapi32.def`（注册表）和 `shell32.def`（托盘）。
2. **文件以换行结尾时，TCC 会把最后一个顶层定义当宏指令吃掉**，
   于是「最后一个函数（`Log`）变成一行声明」→ 后面全部 parse error。
   所以上面的命令写的是 `autoclip2.c`，构建时我额外生成了一个
   末尾补注释、无结尾换行的 `_final.c` 再编译（源码本身可读性不受影响）。
3. TCC 对 `for (i = 0; i < (int)(sizeof(a) / sizeof(a[0]));` 这种
   「括号 + sizeof 除法」表达式会误判成声明，已改成宏 `FMT_COUNT`。
4. TCC 的 `NIDATA` 联合体不支持 `ver.uVersion` 写法，必须 `ver.u.uVersion`。

---

## 五、已知限制

- 交叉编译用的是 TCC，exe 会导入 `msvcrt.dll`（Windows 自带，所有版本都有），
  没做 `/MT` 静态 CRT。
- 没做 UAC 提升。目标程序以管理员运行时，AutoClip 也要用管理员运行。
- 智能模式判断「编辑框」靠窗口类名（Edit / RichEdit / Chrome_WidgetWin 等），
  个别自绘编辑器（如某些 IDE）识别不到时，会走「直接粘贴」分支。
