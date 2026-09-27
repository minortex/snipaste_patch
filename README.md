# Snipaste portal 文件选择器

针对本目录的 `snipaste-2.11.3.AppImage`（Linux x86_64，Qt 6.2.4）。
通过 `LD_PRELOAD` 替换程序调用的 Qt 保存文件、打开文件、选择目录接口，
经会话 D-Bus 调用 `org.freedesktop.portal.FileChooser`，不修改原始 AppImage。

## 启动

先完全退出已有 Snipaste 实例，然后在桌面会话的终端运行：

```sh
./snipaste-portal.sh
```

需要系统已安装并启动 `xdg-desktop-portal` 及支持 FileChooser 的桌面后端，
例如 KDE 或 GTK 后端。实际窗口由当前 portal 后端决定。
已有实例不会被启动脚本注入，必须退出后重新启动。

如果要直接修改当前安装的版本，直接改`/usr/bin/Snipaste`，在 exec 前加`export LD_PRELOAD="/opt/snipaste/lib/libsnipaste-portal.so${LD_PRELOAD:+:$LD_PRELOAD}"`，然后把 so 拷进去。

## 编译

```sh
./build.sh
```

需要 C++17 编译器、Qt 6.2.4 开发文件；编译链接使用 AppImage 自带的 Qt 库。
脚本在缺少开发文件时会给出 `uvx` 下载命令。
可以通过 `SNIPASTE_QT_DIR` 指定其他位置的同版本 Qt 开发文件。
下载缓存、解包文件、编译临时文件及产物均放在本目录。

## 行为与边界

- 传递窗口标题、默认文件名、初始目录和格式筛选；返回选中的本地文件路径及格式。
- 用户取消时直接返回取消，不弹出旧选择器。
- portal 不可用、请求失败或返回非本地 URI 时，回退到原始 Qt 文件选择器，并向终端输出原因。
- portal 打开期间普通贴图保持显示，只临时取消原生窗口的置顶标志；返回后恢复。全屏截图覆盖层仍临时隐藏，释放输入抓取并阻止重复请求。
- X11/XWayland 下传递可见父窗口标识，使 portal 覆盖在贴图上。隐藏的截图父窗口及原生 Wayland 使用无父窗口请求。
- 保存流程由 portal 处理覆盖确认；`DontUseNativeDialog` 不会阻止替换。其他 Qt 专属选项没有逐项映射。
- 仅拦截当前二进制实际引用的三个静态 QFileDialog 接口，不替换任意自定义 QDialog。
- 加载后会从自身的 `LD_PRELOAD` 环境中移除本库，避免 Snipaste 启动的 `wlhelper`（Wayland 剪贴板服务）继承该注入。

## 手动验证

按项目要求未运行功能测试。请启动后截图并选择另存为，检查：

1. 是否出现桌面 portal 文件选择器，文件名、目录、格式是否正确。
2. 保存后图片是否正常生成；切换格式后扩展名及实际图片格式是否符合预期。
3. 取消后是否正常回到 Snipaste，且没有第二个选择器。
4. 选择已存在的文件时是否有覆盖确认。
5. 截图框选后直接保存，portal 是否可见且能操作；取消文件选择后截图选区能否继续编辑和再次保存。
6. 贴图右键另存为时，贴图是否始终显示；保存和取消后贴图是否保留且恢复原来的置顶状态。

若仍出现旧窗口，请提供终端中的 `[snipaste-portal]` 输出及桌面环境信息。
恢复原行为只需退出此实例，直接启动原始 AppImage。
