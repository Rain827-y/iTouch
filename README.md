# iTouch

用电脑通过 SSH 控制已越狱的 iOS 10 手机。已在 iPhone 6 Plus（iPhone7,1，iOS 10.3.3）上通过 USB 实测。

## 连接和操作

手机保持越狱、解锁并信任电脑。在本目录打开 PowerShell，插着数据线执行：

```powershell
python .\itouch.py ping
```

返回 `OK iTouch/1` 表示手机服务在线。如果尚未安装，请先执行 `python .\itouch.py install`。首次连接时，客户端会显示设备的 SSH 主机密钥指纹供确认，然后提示输入 SSH 密码。

```powershell
python .\itouch.py tap 0.5 0.5
python .\itouch.py swipe 0.8 0.65 0.2 0.65 400
python .\itouch.py hold 0.5 0.5 700
python .\itouch.py shell
```

`tap` 点击，`swipe` 滑动，`hold` 长按。默认以当前画面左上角为原点，坐标在 0 到 1 之间。时间单位是毫秒，范围为 30 至 5000。省略滑动时长时用 350 毫秒。`shell` 只登录一次，每行输入一条命令，例如 `tap 0.5 0.5`，输入 `exit` 退出。

快速翻页可用 `python .\itouch.py swipe 0.8 0.77 0.2 0.77 65`。iTouch 1.0.1 在这台 iPhone 6 Plus 上实测该动作的手机端命令耗时约 70 至 80 毫秒。

用截图像素定位时，传入截图的宽高。USB 模式会查询手机当前界面方向，这与连接指南里的 USB 截图脚本使用的是同一方向信息。iPhone 6 Plus 的截图通常是竖屏 `1242x2208` 或横屏 `2208x1242`。应用切换方向后，用当前画面的尺寸重新执行。

```powershell
python .\itouch.py --size 2208x1242 tap 1420 200
python .\itouch.py --size 2208x1242 swipe 1750 950 530 950 430
```

Windows 的 USB 模式需要 Apple Mobile Device Service，例如通过 iTunes 安装。电脑端依赖可通过 `python -m pip install paramiko pymobiledevice3` 安装。已有离线依赖时，可用 `--tools <目录>` 指定包含 `ios-python` 和 `ssh-python` 的工具目录。USB 默认连接手机的 SSH 端口 `2222`，启用了端口 22 的设备可用 `--port 22`。连着多台设备时可用 `--udid <设备序列号>` 选择。

也可以通过 Wi-Fi SSH 连接。把示例 IP 换成手机的 Wi-Fi 地址，按手机实际监听端口设置 `--port`。从 iTouch 1.1.1 起，Wi-Fi 模式默认通过 SSH 查询手机当前的界面方向。需要固定方向时可传入 `--orientation`。

```powershell
python .\itouch.py --host 192.0.2.10 --port 2222 tap 0.5 0.5
```

使用已有的 SSH 终端时，可以直接运行 `/usr/local/bin/itouch ping`、`/usr/local/bin/itouch tap X Y`、`/usr/local/bin/itouch hold X Y MS` 或 `/usr/local/bin/itouch swipe X1 Y1 X2 Y2 MS`。这些命令使用手机原生竖屏的归一化坐标。电脑端 Python 客户端会自动完成方向换算和截图像素换算。

## 查询界面方向

```powershell
ssh -p 2222 root@192.0.2.10 itouch orientation
python .\itouch.py --host 192.0.2.10 --port 2222 orientation
```

成功时只输出一行数字。查询失败时返回非零退出码。方向由 SpringBoard 提供，跟随当前界面。

| 返回值 | 方向 |
|---|---|
| `1` | 竖屏 |
| `2` | 倒竖屏 |
| `3` | 横屏，主屏键在右 |
| `4` | 横屏，主屏键在左 |

Python 可在同一条已认证 SSH 连接上调用 `itouch.ssh_orientation(ssh)` 拿到整数，供 `native_point(x, y, orientation)` 使用。

## 主屏幕键

iTouch 1.2.1 支持模拟单按一次 Home 键。在已解锁的手机上，可以用它从前台应用回到桌面。

```powershell
ssh -p 2222 root@192.0.2.10 itouch home
python .\itouch.py --host 192.0.2.10 home
```

GUI 可以调用 `touch.home()`。通过原始实时 SSH 通道时，发送一行 `home`。同一通道上如果有手指按住，手机会先抬起触点，再按 Home。成功提交按键事件后返回 `OK`。

## 实时拖动接口

iTouch 1.1.0 提供 `itouch.TouchStream(ssh)`，其中 `ssh` 是 `itouch.connect(args)` 返回的已认证 SSH 连接。保持该连接和流会话打开，用 `down(x, y)`、`move(x, y)`、`up()` 分别处理鼠标左键按下、拖动和松开。连接和握手可以在后台线程完成。输入方法只排队，由后台线程收发确认。连续排队的移动点会合并到最新位置，按下和抬起会保留顺序。

```python
from itouch import TouchStream, native_point

with TouchStream(ssh) as touch:
    # 下面三行分别放进 GUI 的按下、拖动和松开回调。
    touch.down(*native_point(x0, y0, orientation))
    touch.move(*native_point(x1, y1, orientation))
    touch.up()
    # 仅用于命令行测试或后台线程同步，GUI 的鼠标回调无需调用。
    touch.flush()
```

`x0, y0` 和 `x1, y1` 是相对画面内容区域的归一化坐标。界面方向值与 `usb_orientation(device)` 相同。鼠标停住时暂停发送移动点即可，Python 客户端会自动维持按住。松开后，App 可按自己的规则执行翻页吸附或惯性滚动。窗口失焦、捕获丢失或界面旋转时可调用 `up()` 结束当前手势。退出流会话或掉线时，手机会释放触点。

其他语言可通过 SSH 执行 `/usr/local/bin/itouch stream`，保持标准输入和输出打开。握手返回 `OK stream/1`。后续每行发送一条 `down X Y`、`move X Y`、`up`、`home`、`ping`、`scroll STEPS [X Y [INTERVAL_MS]]` 或 `scroll-drag STEPS [X Y [INTERVAL_MS]]`。每条成功命令返回一行 `OK`，错误返回 `ERR …` 并结束流。`down` 和 `move` 使用手机原生竖屏的 0 到 1 坐标。手动触点按住且没有移动时，每秒发一次 `ping`，手机在最后一条有效指令后 3 秒自动抬起并关闭流。

## 鼠标滚轮

iTouch 1.2.0 在同一个 `TouchStream` 上提供 `touch.scroll(steps, x=0.5, y=0.5)`。把每次鼠标滚轮事件的格数传进来。正数向上浏览，负数向下浏览，每次可传 1 到 8 格。可选的 `x`、`y` 是鼠标在正向显示画面中的归一化位置，范围为 0 到 1，手机会根据当前界面方向换算。先松开左键拖动，再发送滚轮事件。

iTouch 1.3.0 可选 `mode="drag"` 跟手模式。同一个 SSH 实时会话中，连续滚轮事件会移动同一个触点。停滚后，手机让触点在原处停约 140 毫秒，再自动抬起，以减少 App 的惯性滚动。触点到达可移动区域边缘后会重新落点。默认 `mode="flick"` 为短滑模式，每次移动后立即抬起。

```python
# 放在 GUI 的滚轮回调中。WheelDelta 为 120 时代表向上一格。
if steps := int(wheel_delta / 120):
    touch.scroll(max(-8, min(8, steps)), cursor_x, cursor_y, mode="drag")
```

Python 按收到滚轮事件的间隔调节速度，快滚时手势移动得更远、更快。相邻滞留的同向事件会合并，一次合并最多 8 格。对发送原始协议的客户端，`INTERVAL_MS` 是两次滚轮事件的间隔，范围为 20–200 毫秒，省略时用 160 毫秒。高精度触控板的部分格数可在 GUI 端累计成完整格数后再发送。

```powershell
python .\itouch.py --host 192.0.2.10 scroll -1
python .\itouch.py --host 192.0.2.10 scroll --mode drag -1
ssh -p 2222 root@192.0.2.10 itouch scroll-drag 1 0.5 0.6 35
```

单次 CLI 命令会执行一次拖动并等待抬起。要让连续滚轮事件共享触点，请用 `TouchStream` 或在现有实时通道中发送 `scroll-drag`。

## 安装与卸载

```powershell
python .\itouch.py install
python .\itouch.py uninstall
```

`install` 通过 SSH 上传 `dist\com.itouch.remote_1.3.0_iphoneos-arm.deb`，执行 `dpkg -i` 并检查服务是否启动。Wi-Fi 安装时在 `install` 前加 `--host` 和手机 IP。已有 SSH 会话也可以把包上传后在手机上运行 `dpkg -i <包路径>`。包内程序是 arm64 Mach-O，`iphoneos-arm` 是这台手机上 dpkg 使用的包架构名。

手机端服务由 `launchd` 管理，日志在 `/var/log/itouch.log`。若设备重启后需要重新激活越狱，激活后先运行 `ping`。若 SSH 正常但服务未加载，可以通过手机 SSH 执行 `launchctl load /Library/LaunchDaemons/com.itouch.remote.plist`。

## 技术实现与构建

iOS 10 的触控事件由 IOKit 的 IOHID 接口提交给系统，再交给当前前台界面。服务用 `com.apple.private.hid.client.event-dispatch` 权限发送手指按下、移动和抬起。滑动以手机的单调时钟安排移动和抬起，移动的目标采样率为每秒 120 次。设备上只监听 `/var/run/itouch.sock`，权限为 `0600`。电脑的命令先经过现有 SSH，再由手机端命令转发给服务。安装和运行不需要向 SpringBoard 或应用进程注入 dylib。

构建需要有 Mach-O 支持的 LLVM、Darwin C 头文件和 `rcodesign`。本机使用 Zig 工具链的 Darwin 头文件、LLVM 的 Clang 和 LLD、Android NDK 附带的 `llvm-nm`，以及连接指南目录内的 `rcodesign`。签名使用 SHA-1，以兼容实测的 iOS 10.3.3 环境。

```powershell
python .\build.py --clang <clang路径> --lld <ld64.lld路径> --llvm-nm <llvm-nm路径> --headers <any-darwin-any路径> --signer <rcodesign路径>
```

输出为 `dist\itouch` 和 `dist\com.itouch.remote_1.3.0_iphoneos-arm.deb`。IOHID 事件字段和提交方式可对照 [SimulateTouch](https://github.com/iolate/SimulateTouch)。
