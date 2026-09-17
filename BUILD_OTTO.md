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

## 文件范围

包含 `main/` 源码/音效、`partitions/`、构建脚本、`sdkconfig.defaults.otto`、依赖锁定文件。排除 server、构建输出、自动下载依赖、工具链、ZIP 副本、密钥与本机配置。依赖和编译输出会在新电脑上重新生成，所以源码体积远小于原工作目录。

修改配置时用 `idf.py menuconfig`。已有 `sdkconfig` 优先于 defaults，更新默认配置不会自动覆盖旧配置；建议新克隆验证。
