# Otto 源码编译与烧录

此仓库是四舵机 Otto 固件源码，不包含小智 server。基于小智 ESP32 项目，保留原许可证与上游文档。

## 环境

- ESP-IDF **v5.5.5**，目标 ESP32-S3；不要使用 Arduino 构建。
- Windows 使用已激活 v5.5.5 的 ESP-IDF PowerShell/终端；macOS/Linux 先加载该版本的 `export.sh`。
- 首次构建需要联网下载组件。建议 Windows 克隆到短英文路径，例如 `C:\work\otto`。

## 克隆、编译

```sh
git clone git@github.com:howtion0/otto.git
cd otto
idf.py build
```

无需从旧电脑复制 `sdkconfig`、`build/` 或 `managed_components/`。CMake 自动选用 `sdkconfig.defaults.otto`；`dependencies.lock` 固定组件版本。

当前默认：ESP32-S3、16 MB Flash、octal PSRAM、四个腿脚舵机（无手臂）、中文自定义唤醒词“你好奶龙”、本地动作音效。板级引脚和屏幕配置见 `main/boards/otto-robot/config.h`，不是其他 Waveshare 板型的配置。

## 烧录

Windows（把 COM5 换成实际串口）：

```sh
idf.py -p COM5 flash monitor
```

macOS 示例：`idf.py -p /dev/cu.YOUR_PORT flash monitor`。退出串口监视用 `Ctrl+]`。

使用完整 `idf.py flash`，让它按构建地址表烧录程序、分区表和语音资源，不要只写应用 bin。更换机器人需重新配置网络、设备绑定和舵机校准；这些运行时数据不随源码迁移。烧录前确认引脚和硬件一致，机器人架空以避免舵机动作造成碰撞。

## 本地 Otto Master 连接验证

Otto 固件的设备名为 `EVA1`。Wi-Fi 通过 DHCP 成功后，固件会查询电脑的 `master.local`，然后连接 TCP `8765`，发送换行分隔的 JSON。该链路独立于小智的 MQTT/语音协议，只用于验证配网、mDNS、TCP 和双向消息。

macOS 上启动轻量 WebUI：

```sh
python3 scripts/otto_master.py
```

打开 `http://127.0.0.1:8080`。脚本通过系统的 `dns-sd` 注册 `master.local`，如果电脑有多个网卡，可以显式指定局域网地址：

```sh
python3 scripts/otto_master.py --advertise-ip 192.168.1.20
```

WebUI 可查看设备的 `hello`、`heartbeat` 和 IP，也可以发送 `ping`、`status`、`echo` 或任意 JSON。设备默认每 5 秒发送一次心跳；同一集群中即使多个设备都显示为 `EVA1`，服务端仍使用 Wi-Fi MAC 作为唯一设备 ID。

## MQTT / WebSocket Profile

固件 `2.0.7+` 可在保留Wi-Fi和MQTT回滚资料的前提下，通过认证的 `otto_provision` 写入WebSocket Profile及 `preferred_transport`。允许值仅为 `mqtt` 或 `websocket`；已锁定设备必须提交当前设备token，MAC、client ID、Topic和WebSocket token必须互相匹配。切换后需要重启。

WebSocket为按需语音连接：设备待机时不保持Socket，唤醒或开始监听后才连接。hello携带MAC、NVS设备名、固件版本和Otto state/actions/stop能力。小智固件可能在`listen/start`前发送唤醒词预录Opus；Master应有界丢弃这段音频，不能把它当成用户提问，也不能因此断开认证连接。

固件 `2.0.8+` 的诊断链支持带当前设备token的 `voice_wake`，仅用于确定性真机验收；不得记录或回显token。它模拟唤醒，不代表真实麦克风唤醒词已经通过。

## 固件 2.0.11 循环对话约定

- 待机时按一次机身按钮进入对话；连接中、监听中或播报中再按一次，会关闭当前音频通道并退出到待机。退出后再次按键可建立新会话。
- 同一音频会话内由 Otto Master 循环执行“本地大笑约 2 秒 → 监听 → ASR → DeepSeek → 流式 TTS → 再次大笑”。进入监听后连续 8 秒没有检测到讲话，服务端关闭会话。
- 设备在监听期间发送 `listen/vad`，其中 `speaking=true/false` 表示本地语音活动；按钮退出时，MQTT 和 WebSocket 都会先发送 `goodbye`，供服务端及时清理会话。
- `nailong_laugh.ogg` 使用约 60 ms 的 Opus 包。播放器会等压缩队列、解码、PCM 和 I2S 输出真正排空后才结束本地播放，避免下一段 TTS 重置解码器造成笑声卡顿。
- 动作目录中的 `laugh` 在整段本地音频结束前保持 `moving`/`sound.busy=true`，随后稳定切回 `idle`，避免控制端因错过瞬时状态而等待 15 秒安全超时。

Otto Master `test0.9`已经实现DeepSeek流式`tool_calls`、设备动作目录收窄、参数二次校验和到既有Dispatcher的受控桥接。自然语言前进、后退和大笑只在当前语音设备上执行；成功必须等待Dispatcher真实`completed`，不能把模型文本或单个ACK当作动作完成。多设备并发语音工具隔离仍需在下一检查点验收。

## 文件范围

包含 `main/` 源码/音效、`partitions/`、构建脚本、`sdkconfig.defaults.otto`、依赖锁定文件。排除 server、构建输出、自动下载依赖、工具链、ZIP 副本、密钥与本机配置。依赖和编译输出会在新电脑上重新生成，所以源码体积远小于原工作目录。

修改配置时用 `idf.py menuconfig`。已有 `sdkconfig` 优先于 defaults，更新默认配置不会自动覆盖旧配置；建议新克隆验证。
