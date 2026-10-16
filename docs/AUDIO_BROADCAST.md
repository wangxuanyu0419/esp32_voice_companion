# N100 音频广播兼容

`tools/n100/audio_broadcast_compat.py` 来自本地 9 月发送端脚本。它是辅助模块，不自行发送 HTTP 请求。运行环境为 Linux/POSIX、Python 3.10+（使用 fcntl）、PATH 中的 FFmpeg / FFprobe。Windows 可检查语法，不能原生运行。

## 集成

simple M4A 解码器需要普通／faststart MP4。模块在有限头尾窗口内检查 mvex/moof 标记，并对可能到达 esp32s3_001 的 fMP4 执行无损 copy-remux。指定其他设备则保留源文件；不指定设备视为全设备广播，也需准备 ESP32 兼容容器。

让调用脚本能够导入此模块，然后使用：

```python
from audio_broadcast_compat import prepared_audio_for_broadcast

with prepared_audio_for_broadcast(source_path, device_id="esp32s3_001") as prepared:
    # 在这里调用原有的已认证 /audio-broadcast 请求。
    # file_path 使用 str(prepared)，必须是服务端可读取的路径。
    submit_audio_broadcast(file_path=str(prepared), device_id="esp32s3_001")
```

submit_audio_broadcast 为示意函数，仓库不提供该函数。模块不上传文件；跨机器集成需先保证服务端可读文件路径。检测是有界标记扫描，不是完整 atom 解析；copy-remux 不改变编码。

## 边界

| 项目 | 实现 |
| --- | --- |
| 输入 | ESP32 准备路径最多 64 MiB；转封装探测最多 15 分钟 |
| 探测 | 文件头尾各最多 1 MiB |
| 超时 | FFprobe 5 秒、FFmpeg 60 秒 |
| spool | 最多 16 文件／128 MiB，满时拒绝，不提前驱逐 |
| 保留 | AUDIO_BROADCAST_TTL_MS + 5 分钟，默认 6 小时 + 5 分钟 |
| 清理 | 后续转封装／容量检查触发，不是独立定时服务 |
| 失败 | AudioCompatibilityError；不得回退发送已知不兼容的原文件 |

CLAWCHAT_ESP32_AUDIO_SPOOL_DIR 可覆盖默认 ~/.hermes/cache/clawchat-esp32-audio-remux。TTL 优先读环境，其次读保留的部署路径 /home/xuanyu/clawchat-companion/server/.env，最后使用默认值；迁移时应显式配置。

上下文退出后输出仍保留，因为设备可能稍后才拉取；不要在 POST 返回后立即删除。本次没有部署 N100 服务或更改线上广播任务。
