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

- `self.otto.laugh`：无参数，只安排播放约 2 秒的 `nailong_laugh.ogg`（奶龙大笑），不触发舵机。MCP 工具调用在成功入队后立即返回，不代表音频已经播完；控制端应观察 `sound.busy`。
- 动作目录同时暴露 `laugh`，供本机 Otto Master 在每轮提问前调用。这个动作会在笑声音频真正播放完毕前保持 `moving` 和 `sound.busy=true`，然后切回 `idle`；若音频暂忙，会以 20 ms 间隔有界等待，最多约 5 秒。
- 动作音效由 `otto_sound` 任务送入音频播放队列，动作任务不再等整段音频送完才运动。
- 同时只接受一个本地音效。普通运动音效遇忙时跳过，运动仍照常执行，避免积压；只有独立 `laugh` 动作使用上述有界重试并等待播放完成。
- 行走、摇摆、转向/跳跃、弯曲/摇腿和其他动作的音效映射保持原有设置。

目标人设要求：听不懂时调用 `self.otto.laugh` 一次，成功后不再追加语音；工具不可用时退回文字笑声。Otto Master `test0.9`已经向DeepSeek声明当前设备的受限工具，并把流式`tool_calls`经参数校验后送入既有Dispatcher；EVA1的自然语言大笑和行走已验证。只有持久命令`completed`才算成功，模型文字和单个ACK仍不能作为动作证据。

机身按钮采用会话开关语义：待机时按下进入循环对话，连接、监听或播报期间按下退出；退出后再次按下建立新会话。每轮回答播放完后由服务端重新触发约 2 秒大笑并继续监听，监听 8 秒无人讲话则自动退出。

“你好奶龙”待机唤醒由固件处理；“奶龙在呢！”由云端人设回应。当前服务端固定使用火山湾区大叔音色；人设语气仍保持活泼、呆萌、简短，不采用大叔式措辞。
