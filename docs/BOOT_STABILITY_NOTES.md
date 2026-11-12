# 开机稳定性经验（Metalio Claw4 / ESP32-P4）

## 1. 串口一开就"重启"，多半不是固件

- 日志里出现 `rst:0x17 (CHIP_USB_UART_RESET)` 表示复位来自 USB 串口（DTR/RTS），
  不是看门狗或代码崩溃。
- 反复打开/关闭串口、浏览器刷机页仍占用 WebSerial、串口监视器自动重连，
  都会持续复位芯片，看起来像"无限重启"。
- 抓日志要**只打开一次**，失败也不要循环重连。

## 2. 整包刷写必须校验通过

- 20MB 的整包从 `0x0` 写入，任何中途断开都会留下半截镜像；
  启动时会出现 `SHA-256 comparison failed` 并进入反复重启。
- 判定标准只有一条：esptool 输出 `Hash of data verified.`。
- 整包刷写会清空 NVS，刷完需要重新配网。

## 3. 不要在 PSRAM 栈的任务里写 Flash / NVS

- 症状：`assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
  cache_utils.c:141 (esp_task_stack_is_sane_cache_disabled())`。
- NVS 写入会关闭 Cache，此时 PSRAM 栈不可访问，必然断言。
- 处理：`Settings` 的所有写操作已改为在内部 RAM 栈的临时任务上执行
  （`main/settings.cc` 的 `RunNvsWrite`）。

## 4. 不要关闭这两个配置

- `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`
- `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`

板级/组件里存在 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` 的静态任务，
关闭上述选项会在启动阶段断言（`xPortcheckValidStackMem`），表现为黑屏重启。

## 5. 默认网络保持 WiFi

整包刷机后 NVS 为空，若默认走 4G 会在模组初始化阶段写 NVS 而卡在启动流程。
`home_screen.cc` 与 `wifi_required_dialog.cc` 的默认网络类型保持 `WiFi(0)`。

## 6. 音频播放注意

- 签名 URL 的 JWT 含 `.`，按扩展名选解码器时必须保证最后一个 `.` 是 `.mp3`。
- 重采样目标要在 pool 上设置（`aud_rate_cvt`），只在 pipeline 上查往往查不到。
- HTTP IO 需要 `get_score` 覆盖，明确把 `http(s)://` 交给 HTTP IO。
