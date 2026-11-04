# wireless-esp32-dap → ESP32-S3-BOX-3 (Dock/PMOD2) 移植分析

> 分析时间：2026-11-04　目标工程：`D:\ESP_Prj\wireless-dap-s3-box`
> 上游：https://github.com/badboyhuan/wireless-esp32-dap （fork 自 windowsair/wireless-esp8266-dap）
> 本机：ESP-IDF v5.5.4 / Windows / COM3（USB-Serial/JTAG）

---

## 1. 结论摘要

1. **这不是"改两个引脚号"的移植**。上游代码只支持 `ESP8266 / ESP32 / ESP32C3` 三个目标，全部走 **ESP-IDF 4.4** 的旧 API；本机是 **IDF 5.5.4**。所以需要同时完成两件事：
   - **A. 增加 ESP32-S3 硬件抽象层**（GPIO 位操作 / SPI 加速 / 定时器 / 引脚表）；
   - **B. 把 IDF 4.4 代码迁移到 IDF 5.5**（旧 `system_event` 事件循环、`tcpip_adapter`、mDNS、timer_group、CMake 组件注册）。
2. **好消息：USB 部分不需要移植**。该项目的 USBIP 是**纯软件实现**（`lwip` socket + 手工构造 USB 描述符/URB 回包），不依赖 ESP32 的 USB Device 外设，因此 ESP-IDF 5.x 移除 legacy USB 栈、改用 TinyUSB **完全不影响本项目**。
3. **硬件侧唯一真正的新工作**是 SWD 的底层时序：GPIO 位操作层（必须）＋ SPI 40MHz 加速层（可选，S3 上需要额外处理）。
4. 建议分 5 个里程碑推进，**先跑通"GPIO 模式 + Wi-Fi + usbip attach + 读出目标 IDCODE"**，再考虑 SPI 提速。

---

## 2. 已核实的环境与硬件事实

| 项目 | 实测结果 |
|---|---|
| 芯片 | ESP32-S3 (QFN56) rev v0.2，双核 240MHz |
| Flash | **16MB**（Manufacturer c8, Device 6018） |
| PSRAM | 内嵌 16MB，1.8V（AP_1v8） |
| eFuse | Flash 4 线（quad），3.3V |
| MAC | 74:4d:bd:2e:88:50 |
| USB | USB-Serial/JTAG（原生 USB，**不是 CH340**），枚举为 **COM3** |
| ESP-IDF | v5.5.4，位于 `D:\ESP32_SDK\esp32s3_v554\.espressif\v5.5.4\esp-idf` |
| 工具链 | `C:\Espressif\tools`（xtensa-esp-elf esp-14.2.0_20260121 / cmake 3.30.2 / ninja 1.12.1） |
| Python 环境 | `C:\Espressif\tools\python\v5.5.4\venv`（含 esptool 4.12.0、idf_component_manager） |
| 代理 | 系统代理 `127.0.0.1:7890` 可用；git 全局代理 `192.168.168.4:8080` **已失效**（需临时覆盖） |

**已修复的环境问题**：`export.ps1` 原报错找不到 `C:\Espressif\python_env\idf5.5_py3.14_env`，实际 venv 在 `C:\Espressif\tools\python\v5.5.4\venv`；且 `espidf.constraints.v5.5.txt` 缺失。
已验证可用的环境变量组合：

```powershell
$env:IDF_TOOLS_PATH='C:\Espressif'
$env:IDF_PATH='D:\ESP32_SDK\esp32s3_v554\.espressif\v5.5.4\esp-idf'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v5.5.4\venv'
# PATH 追加: venv\Scripts; xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;
#            cmake\3.30.2\bin; ninja\1.12.1; idf-exe\1.0.3
```

`idf.py --version` → `ESP-IDF v5.5.4`（exit 0）。

---

## 3. 板子与引脚

`D:\ESP_Prj\esp-box`（espressif/esp-box，已更新到 IDF 5.5 分支）中 BOX-3 的 BSP 定义：

```
BSP_PMOD2_IO1 = GPIO13   ; 用户指定为 SWDIO
BSP_PMOD2_IO2 = GPIO9
BSP_PMOD2_IO3 = GPIO12   ; 建议的 SWCLK（FSPICLK）
BSP_PMOD2_IO4 = GPIO44   ; UART0 RX
BSP_PMOD2_IO5 = GPIO10   ; 用户指定为 SWCLK
BSP_PMOD2_IO6 = GPIO14
BSP_PMOD2_IO7 = GPIO11   ; 建议的 SWDIO（FSPID/MOSI）
BSP_PMOD2_IO8 = GPIO43   ; UART0 TX
```

同时 `GPIO13` 也是 SD 卡 `SD_D1`（不插卡无冲突），`GPIO10` 为 `PMOD2_IO5`。

**关键发现（影响 SPI 加速方案）**：ESP32-S3 的 IO_MUX 直连 SPI2 引脚是
`FSPID=11 / FSPICLK=12 / FSPIQ=13 / FSPICS0=10`。
- 选定的 **GPIO10 = FSPICS0**，**GPIO13 = FSPIQ（MISO 输入）**，两者都不能直接当 SPI 时钟/数据输出；
- 因此若坚持 10/13，40MHz SPI 加速只能**走 GPIO 交换矩阵（gpio_matrix_out/in）**，速率与稳定性会打折；
- 若允许把线改到 **P11(SWDIO) + P12(SWCLK)**（同在 PMOD2 上，IO7/IO3），SPI 加速可用 IO_MUX 直连，几乎零额外代价。
- 若只有 2 根线：`DAP_JTAG` 应设为 **0**（无 TDI/TDO/nTRST），nRESET 可另选 P9 或 P14。

---

## 4. 上游代码现状

- `badboyhuan` fork = 上游 master + 1 个提交（`fix wrong BOS response length in Linux (#49)`），**不含 S3 支持**。
- 目录：`components/DAP`（CMSIS-DAP 协议栈）、`components/USBIP`（USBIP 服务端 + 描述符）、`components/kcp`、`components/elaphureLink`、`main/`（wifi/tcp/dap 调度）。
- 目标相关分支分布在 **12 个文件**：`DAP_config.h`、`gpio_common.h`、`gpio_op.h`、`DAP.h`、`DAP.c`、`SW_DP.c`、`spi_op.c`、`spi_switch.c`、`timer.c`、`uart_bridge.c`、`wifi_handle.c`、`main.c`。
- 构建系统是 IDF 3.x 风格 `register_component()`（IDF 5.5 仍保留兼容宏，但 `PROJECT_PATH` 变量需替换）。
- 缺失子模块：`components/corsacOTA`（OTA，`USE_OTA=0` 默认关闭，可不引入）。
- 数据通路：主机 `usbip` 客户端 → TCP:3240 → `usbip_server.c` → `DAP_handle.c` → `DAP.c`（CMSIS-DAP 命令）→ `SW_DP.c`/`JTAG_DP.c` → `spi_switch.c`/`spi_op.c`(SPI 模式) 或 `DAP_config.h`(GPIO 模式)。

---

## 5. 工作分解（WBS）

### A. 工程骨架与构建系统（必做）
| 文件 | 改动 |
|---|---|
| `CMakeLists.txt` | `cmake_minimum_required(VERSION 3.16)`；去掉无用 `include_directories` |
| `main/CMakeLists.txt` | `register_component()` → `idf_component_register(SRCS ... INCLUDE_DIRS ".." ".")`；`REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip esp_timer driver` |
| `components/*/CMakeLists.txt` | 同上迁移，`REQUIRES` 明确化（DAP、USBIP、kcp） |
| `.gitmodules` / `ESP8266_RTOS_SDK` | 移除（S3 不需要） |
| `components/corsacOTA` | `USE_OTA=0`，不上子模块；如需 OTA 再引入 |
| `main/idf_component.yml` | 新增 `espressif/mdns`（IDF 5.x mDNS 已移出 IDF） |

### B. IDF 4.4 → 5.5.4 API 迁移（必做）
| 文件 | 问题 | 改法 |
|---|---|---|
| `main.c` | `#include "esp_event_loop.h"`（5.x 已删） | 改 `esp_event.h`；`app_main` 增加 `esp_netif_init()` / `esp_event_loop_create_default()` |
| `wifi_handle.c` | 旧 `system_event_t` + `esp_event_loop_init` + `tcpip_adapter_*` + `ESP_IF_WIFI_STA` | 重写为 `esp_netif_create_default_wifi_sta()` + `esp_event_handler_instance_register(WIFI_EVENT/IP_EVENT)`；静态 IP 用 `esp_netif_set_ip_info()` + `esp_netif_dhcpc_stop()`；`ESP_IF_WIFI_STA`→`WIFI_IF_STA` |
| `tcp_server.c` / `tcp_netconn.c` / `kcp_server.c` / `uart_bridge.c` | 只为 `esp_event_loop.h` include | 改 `esp_event.h`；UART 驱动 API 在 5.x 仍存在，需核对 `uart_param_config`/`uart_driver_install` 与 `UART_SCLK_*` |
| `timer.c` | 只有 ESP8266 有实现，ESP32/C3 返回 0 | 用 **gptimer**（1MHz、向上计数）或 systimer 实现 `get_timer_count()`，S3 与 ESP32 共用新实现（TIMESTAMP_CLOCK=5MHz） |
| `main.c` | `mdns_init()` 等 | 改用 managed component `espressif/mdns`，API 基本兼容 |
| 全项目 | `os_printf` 由 `wifi_configuration.h` 本地实现 | 保留即可；建议逐步换 `ESP_LOGI` |

### C. ESP32-S3 硬件抽象层（必做，核心工作量）
| 文件 | 改动 |
|---|---|
| `components/DAP/include/gpio_common.h` | 新增 `CONFIG_IDF_TARGET_ESP32S3` 分支：`soc/gpio_struct.h`、`soc/spi_struct.h`、`soc/spi_reg.h`、`hal/gpio_ll.h`、`soc/io_mux_reg.h`、`soc/gpio_sig_map.h`（IDF 5.x 头文件路径已扁平化，S3 的寄存器头在 `soc/esp32s3/register/soc/`） |
| `components/DAP/include/gpio_op.h` | 新增 S3 分支：`gpio_ll_set_level/get_level`、`gpio_ll_output_enable/input_enable`、`gpio_ll_pullup_en/pulldown_dis`、`gpio_ll_func_sel`。S3 的 `GPIO.enable_w1ts` 为 union（同 C3），可直接复用 C3 写法 |
| `components/DAP/config/DAP_config.h` | 新增 S3 段：`CPU_CLOCK 240000000`；`PIN_SWDIO_MOSI=13`、`PIN_SWCLK=10`、`PIN_SWDIO=13`；`PIN_TDI/TDO/nTRST` 置 `_`；`PIN_nRESET` 可选 P9/P14；`PORT_JTAG_SETUP/PORT_OFF/PIN_*` 各内联函数补 S3 分支；`DAP_JTAG` 建议设 0 |
| `components/DAP/source/DAP.c` | 补 S3 的 `BUS_CLOCK_FIXED`（240MHz 主频，APB 80MHz → 用 80MHz 更保守） |
| `components/DAP/source/DAP.h` | 条件编译链加 S3 |
| `components/DAP/source/SW_DP.c` | WAIT/FAULT 时的时钟生成：S3 按 C3 走 `DAP_SPI_Fast_Cycle()` |
| `components/DAP/source/spi_op.c` | `#define DAP_SPI GPSPI2`；S3 与 C3 同为 GPSPI2，`ms_dlen`/`user2.usr_command_bitlen` 语义一致，基本可照搬 C3 分支 |
| `components/DAP/source/spi_switch.c` | S3 的 `DAP_SPI_Init()`：`periph_ll_enable_clk_clear_rst(PERIPH_SPI2_MODULE)`；**引脚用 GPIO 矩阵**（`gpio_matrix_out(10, FSPICLK_OUT_IDX)` / `gpio_matrix_out(13, FSPID_OUT_IDX)` / `gpio_matrix_in(13, FSPID_IN_IDX)`），而非 IO_MUX |
| `main/wifi_handle.c` | `PIN_LED_WIFI_STATUS`：BOX-3 无独立用户 LED，可置为 `_` 或复用背光引脚 |

### D. 板级配置（必做）
| 文件 | 内容 |
|---|---|
| `sdkconfig.defaults.esp32s3` | **新增**（上游没有）：16MB flash、`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`、240MHz、`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`（日志走 COM3，且避开 GPIO43/44 与 PMOD2 IO4/IO8 冲突）、关闭 Bluetooth、关闭 PSRAM 或保持默认、`CONFIG_ESP_MAIN_TASK_STACK_SIZE` 加大、`CONFIG_LWIP_TCP_*` 调优 |
| `partitions.csv` | 如需 OTA：16MB 下 `nvs + otadata + app*2 + storage`；单 app 亦可 |
| `main/wifi_configuration.h` | 填 SSID/密码；`USE_STATIC_IP` 建议先关（用 DHCP + `dap.local`），默认 `192.168.137.123` 属 Windows ICS 网段，易冲突 |

### E. 构建、烧录、联调（必做）
1. `idf.py set-target esp32s3 && idf.py build`
2. `idf.py -p COM3 flash monitor`（USB-Serial/JTAG，注意 BOOT 键进下载模式）
3. PC 侧：安装 **usbip-win 0.3.0（kmdf/ude）**，`usbip attach_ude -r <ip> -b 1-1`
4. 用 Keil MDK 或 OpenOCD（`cmsis-dap`）验证：能看到 CMSIS-DAP 设备、能读目标 `IDCODE`

### F. 可选增强
- SPI 40MHz 加速（取决于引脚方案，见 §3）
- nRESET 硬复位（PMOD2 IO2=P9 / IO6=P14）
- UART TCP Bridge（`USE_UART_BRIDGE=1`，需 2 个引脚）
- OTA（引入 corsacOTA 子模块）
- 状态 LED / 更详细的日志

---

## 6. 里程碑与验收标准

| 里程碑 | 内容 | 验收标准 |
|---|---|---|
| **M0** 环境 | IDF 5.5.4 可编译空工程 | `idf.py build` 通过，能烧录 `blink` 类工程 |
| **M1** 编译通过 | 完成 A+B（先不含 S3 HAL），临时把目标宏当 C3 编译 | `idf.py set-target esp32s3 build` 无错（允许 S3 相关告警） |
| **M2** 硬件层 | 完成 C+D | 上电打印 Wi-Fi 连接成功 + IP + mDNS |
| **M3** DAP 联调 | usbip attach + SWD | 主机识别 CMSIS-DAP，能读目标 IDCODE / 读写内存 |
| **M4** 稳定 | 长时间调试、烧写验证 | 100KB 固件烧写无失败，全程无丢包重连 |
| **M5** 加速（可选） | SPI 40MHz | `adapter speed 10000` 下 OpenOCD 正常 |

---

## 7. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| IDF 5.x 事件循环/esp_netif 迁移量大 | 中 | 照 `esp-idf/examples/wifi/getting_started/station` 重写 `wifi_handle.c`，逻辑（多 SSID 轮换、静态 IP）保留 |
| S3 的 GPSPI2 与 C3 寄存器仍有差异 | 中 | 先不做 SPI，GPIO 模式验证功能；SPI 用最小 demo 单独验证时序 |
| GPIO10/13 不是 SPI IO_MUX 引脚 | 中 | 走 GPIO 矩阵；或建议改线到 P11/P12 |
| 纯 GPIO 模式速度（约 2–5MHz）不够快 | 低 | STM32 常规调试足够；烧写 100KB 约数秒 |
| usbip-win 驱动安装/签名 | 中 | 需管理员权限；或用 usbip-win 0.3.0 的 kmdf ude 模式 |
| 覆盖 BOX-3 原厂固件 | 低 | 可用 `esp-box` 仓库 factory_demo 恢复；建议先 `esptool read_flash` 备份 16MB |
| 目标板电平/供电 | 低 | PMOD2 提供 3V3/GND；确认与目标板共地、3.3V 电平 |

---

## 8. 开工前需确认

1. **目标调试板**是什么（STM32？3.3V？）——决定 SWD 电平、是否共地、验收方式。
2. **Wi-Fi 方式**：BOX-3 做 STA 连家里路由（需 SSID/密码），还是做 SoftAP 让 PC 直连？
3. 是否可以**擦除 BOX-3 现有固件**（会失去出厂 demo，可恢复）。
4. 是否允许把 SWD 两根线改到 **P11/P12**（可免费获得 40MHz SPI 加速）；若不能，按 10/13 做 GPIO 模式 + 矩阵 SPI。
5. 是否需要 **nRESET**（可接 PMOD2 IO2=P9 或 IO6=P14）。
