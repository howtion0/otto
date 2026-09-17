# 奶龙定制配置

本工作区的无手 Otto 使用中文 MultiNet7 自定义唤醒词：`ni hao nai long`（你好奶龙），初始阈值 20%。实际识别率、误唤醒率需要在实机上验证。

当前 `sdkconfig` 已设置。`sdkconfig.defaults.otto-nailong` 保存同一组配置，供以后重建配置时作为额外 defaults 使用；它不是完整的硬件配置备份。修改 defaults 不会覆盖已有 sdkconfig。

在项目根目录、已激活 ESP-IDF 的终端中编译：

```sh
idf.py build
idf.py -p /dev/cu.YOUR_PORT flash
```

必须同时烧录 `build/generated_assets.bin` 中的 MultiNet 模型和唤醒词索引。`idf.py flash` 会按构建生成的地址表写入，不要只烧录主程序。现有 16 MB Flash、8 MB octal PSRAM、屏幕配置和无手引脚设置保持原样。

## 本地音效

- `self.otto.laugh`：无参数，只安排播放 `action_swing.ogg`（奶龙大笑），不触发舵机。调用后立即返回，不代表音频已经播完。
- 动作音效由 `otto_sound` 任务送入音频播放队列，动作任务不再等整段音频送完才运动。
- 同时只接受一个正在提交的本地音效；忙时新的音效请求跳过，运动仍照常执行，避免积压长音频。
- 行走、摇摆、转向/跳跃、弯曲/摇腿和其他动作的音效映射保持原有设置。

云端人设已要求：听不懂时调用 `self.otto.laugh` 一次，成功后不再追加语音；工具不可用时退回文字笑声。判断“听不懂”及工具调用仍由云端模型完成，不是本地语音识别的确定性规则。完全没有识别结果或断网时，云端可能无法触发此规则。

“你好奶龙”待机唤醒由固件处理；“奶龙在呢！”由云端人设回应。音色仍使用后台原来选定的音色。
