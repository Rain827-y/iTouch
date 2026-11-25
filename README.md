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

也可以通过 Wi-Fi SSH 连接。把示例 IP 换成手机的 Wi-Fi 地址，按手机实际监听端口设置 `--port`，发送动作时指定当前屏幕方向。

```powershell
python .\itouch.py --host 192.0.2.10 --port 2222 --orientation portrait tap 0.5 0.5
```

使用已有的 SSH 终端时，可以直接运行 `/usr/local/bin/itouch ping`、`/usr/local/bin/itouch tap X Y`、`/usr/local/bin/itouch hold X Y MS` 或 `/usr/local/bin/itouch swipe X1 Y1 X2 Y2 MS`。这些命令使用手机原生竖屏的归一化坐标。电脑端 Python 客户端会自动完成方向换算和截图像素换算。

## 安装与卸载

```powershell
python .\itouch.py install
python .\itouch.py uninstall
```

`install` 通过 USB SSH 上传 `dist\com.itouch.remote_1.0.1_iphoneos-arm.deb`，执行 `dpkg -i` 并检查服务是否启动。已有 SSH 会话也可以把包上传后在手机上运行 `dpkg -i <包路径>`。包内程序是 arm64 Mach-O，`iphoneos-arm` 是这台手机上 dpkg 使用的包架构名。

手机端服务由 `launchd` 管理，日志在 `/var/log/itouch.log`。若设备重启后需要重新激活越狱，激活后先运行 `ping`。若 SSH 正常但服务未加载，可以通过手机 SSH 执行 `launchctl load /Library/LaunchDaemons/com.itouch.remote.plist`。

## 技术实现与构建

iOS 10 的触控事件由 IOKit 的 IOHID 接口提交给系统，再交给当前前台界面。服务用 `com.apple.private.hid.client.event-dispatch` 权限发送手指按下、移动和抬起。滑动以手机的单调时钟安排移动和抬起，移动的目标采样率为每秒 120 次。设备上只监听 `/var/run/itouch.sock`，权限为 `0600`。电脑的命令先经过现有 SSH，再由手机端命令转发给服务。安装和运行不需要向 SpringBoard 或应用进程注入 dylib。

构建需要有 Mach-O 支持的 LLVM、Darwin C 头文件和 `rcodesign`。本机使用 Zig 工具链的 Darwin 头文件、LLVM 的 Clang 和 LLD、Android NDK 附带的 `llvm-nm`，以及连接指南目录内的 `rcodesign`。签名使用 SHA-1，以兼容实测的 iOS 10.3.3 环境。

```powershell
python .\build.py --clang <clang路径> --lld <ld64.lld路径> --llvm-nm <llvm-nm路径> --headers <any-darwin-any路径> --signer <rcodesign路径>
```

输出为 `dist\itouch` 和 `dist\com.itouch.remote_1.0.1_iphoneos-arm.deb`。IOHID 事件字段和提交方式可对照 [SimulateTouch](https://github.com/iolate/SimulateTouch)。
