# DSH-Board 固件骨架（ESP-IDF / ESP32-S3）

把板子做成「服务端」：Wi-Fi 配网 + mDNS 广播 + HTTP API + WebSocket PCM 音频 + 配对。

> 这是**骨架**：网络、mDNS、HTTP、配对、WS 帧、PCM 队列、ES8311/ES7210 16kHz 初始化都已补；AEC/VAD/OTA 仍留 TODO。默认 `BOARD_AUDIO_ENABLE=1`，只想调网络时可改成 `0`。

## 目录

```
firmware/
├── CMakeLists.txt
├── partitions.csv          # nvs / factory_nvs / ota_0 / ota_1
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── board_config.h      # 引脚、采样率、开关
    ├── main.c              # app_main、mDNS、按键重置
    ├── wifi_prov.c/.h      # STA + SoftAP 配网、扫描
    ├── pairing.c/.h        # 配对码/token、factory NVS
    ├── http_api.c/.h       # /api/*、配网页、/ws
    ├── ws_audio.c/.h       # hello、PCM 帧、控制消息
    └── audio_i2s.c/.h      # I2S 读写、测试音
```

## 编译

要求 ESP-IDF **5.5.1+**。

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

首次编译前先检查 `main/board_config.h`：

1. 引脚已按板级资料填好（I2S：12/13/14/15/16，I2C：10/11，BOOT：0）；仍建议对照原理图核对。
2. 已补 ES8311/ES7210 的 16kHz/16bit 初始化；默认 `BOARD_AUDIO_ENABLE=1`，上板听声/测麦。只想调网络时改成 `0`。
3. 量产时不要用默认配对码 `123456`，由产线写 `factory_nvs`。

## 运行流程

1. 首次上电：没有 Wi-Fi 凭证 → 开热点 `DSH-Board-XXXX`（密码在串口日志/标签）。
2. 手机连热点，打开 `http://192.168.4.1`，填 2.4G Wi-Fi。
3. 板子重启入网，mDNS 广播 `_dsh-board._tcp`。
4. DSH 插件发现、`/api/pair` 换 token、连 `/ws`。
5. 说「你好」→ 插件收 PCM → TTS 下发 → 板子播放。

## 快速自测

```bash
# 设备信息（AP 或 STA 的 IP）
curl http://192.168.4.1:8080/api/info

# 配对（开发默认码 123456）
curl -X POST http://192.168.4.1:8080/api/pair \
  -H "Content-Type: application/json" \
  -d '{"code":"123456","client_id":"dev","client_name":"curl"}'

# 状态（把 <token> 换成上一步返回值）
curl -H "Authorization: Bearer <token>" http://192.168.4.1:8080/api/status
```

mDNS：

```powershell
# Windows 装了 Bonjour 时
dns-sd -B _dsh-board._tcp
# 或者直接 ping 主机名
ping dsh-board-xxxx.local
```

WebSocket 可以用 `websocat`：

```bash
websocat -H "Authorization: Bearer <token>" ws://192.168.4.1:8080/ws
# 连上后输入：
{"type":"session.start","mode":"push_to_talk"}
{"type":"ping","ts":1}
```

## 默认行为

| 项目 | 值 |
|---|---|
| HTTP / WS 端口 | 8080 |
| mDNS | `_dsh-board._tcp`，TXT：id/name/model/fw/proto/pair/caps |
| 音频 | 16kHz / 16bit / 单声道 / 20ms（640 字节） |
| 配对码 | 开发默认 `123456`，量产必须改写 |
| AP 密码 | 首次启动生成 `dshXXXXX`，写 factory NVS |
| 长按 BOOT 5s | 解除配对（清 token） |
| 长按 BOOT 10s | 清 Wi-Fi + token，重启进配网 |

## 已实现 / 待补

- ✅ Wi-Fi STA + SoftAP、扫描、配网页
- ✅ mDNS 广播与 TXT
- ✅ `/api/info` `/api/pair` `/api/unpair` `/api/status` `/api/config` `/api/reboot`
- ✅ 配对码校验、token 签发/校验、失败锁定
- ✅ WebSocket hello、二进制帧、`session.start/stop`、`abort`、`ping`
- ✅ 上行 PCM 任务、下行 PCM 队列、测试音
- ✅ I2C 探测 ES8311/ES7210/TCA9555 + TCA9555 EXIO8 功放使能
- ✅ ES8311/ES7210 16kHz/16bit 寄存器初始化（待真机验证）
- ✅ `/api/test`（喇叭测试音 + 麦克风 RMS）
- ✅ 配对/改名后 mDNS TXT 即时刷新
- ✅ `/ota/manifest` 204 占位
- ⬜ 真机验证：麦克风增益/音量标定、AEC/VAD/Opus
- ⬜ OTA 实际下载与校验
- ⬜ 产线 NVS 写入（脚本已在 `制作/产线/nvs_factory.py`）

## 已知注意点

- `httpd_ws_send_frame_async()` 的 payload 生命周期依赖底层实现；生产建议改成 TX 队列，发送完成后再释放。
- 默认 `BOARD_AUDIO_ENABLE=1`：会启动 I2S 并初始化 codec；没有 codec 或只想调网络时改成 `0`。
- 配网页面只做了最简单的 SSID/密码表单，没有 captive portal DNS；需要手动打开 `192.168.4.1`。
- 分区表按 4MB 布局；16MB 板子可以放大 `ota_0/ota_1`。
