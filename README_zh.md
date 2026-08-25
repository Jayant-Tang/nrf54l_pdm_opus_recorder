# learning_zephyr_pdm

nRF54L15DK 上基于 Zephyr DMIC API 的最小 PDM 录音和 SoC 功耗测量 Demo。

## 硬件

- **开发板**：nRF54L15DK
- **Board target**：`nrf54l15dk/nrf54l15/cpuapp`
- **NCS**：v3.4.0
- **PDM 外设**：PDM20
- **PCM 输出**：16 kHz、16-bit
- **PDM ratio**：50
- **PDM GPIO drive**：`H0H1`
- **运行方式**：上电后自动开始录音

### PDM 接线

| nRF54L15DK | PDM 麦克风 |
|---|---|
| P1.12 | CLK |
| P1.11 | DIN / DATA |
| GND | GND |
| 外部供电 | VDD |

双麦克风时，两颗麦克风共用 CLK 和 DIN，通过各自的 L/R 脚分别固定为 left 和 right。
单麦克风时，只连接其中一个 L/R 通道，并选择对应的 mono 配置。

P1.11/P1.12 不应与 nRF7002 shield 同时使用。PDM 麦克风供电由外部供电，不纳入本 Demo 的 SoC 功耗统计。

## 功能

1. 上电后自动配置并启动 PDM。
2. 主线程持续调用 `dmic_read()` 获取 PCM DMA block。
3. 当前 Demo 不处理、不保存音频，读取后立即调用 `k_mem_slab_free()` 释放 block。
4. 默认是 stereo；mono 可选择 left 或 right。

Debug 固件每收到 100 个 block 输出一次统计 log；Release 固件关闭 log，适合直接测量持续录音功耗。

## 构建

先通过 nRF Connect SDK toolchain 初始化当前 PowerShell：

```powershell
nrfutil sdk-manager toolchain env --ncs-version=v3.4.0 --as-script powershell |
    Out-String |
    Invoke-Expression
$env:ZEPHYR_BASE = "D:\ncs\v3.4.0\zephyr"
```

### Debug stereo

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp `
    -d build D:\Project\learning_zephyr_pdm --no-sysbuild
```

### Release stereo

`prj_release.conf` 会关闭 `CONFIG_LOG`、`CONFIG_SERIAL`、console 和 UART backend：

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp `
    -d build_release D:\Project\learning_zephyr_pdm --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=D:/Project/learning_zephyr_pdm/prj_release.conf"
```

### Release mono

使用新的 build directory，并叠加 mono 配置：

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp `
    -d build_release_mono D:\Project\learning_zephyr_pdm --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=D:/Project/learning_zephyr_pdm/prj_release.conf" `
       "-DCONFIG_PDM_DEMO_STEREO=n"
```

默认 mono 选择 left；选择 right：

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp `
    -d build_release_mono_right D:\Project\learning_zephyr_pdm --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=D:/Project/learning_zephyr_pdm/prj_release.conf" `
       "-DCONFIG_PDM_DEMO_STEREO=n" `
       "-DCONFIG_PDM_DEMO_MONO_RIGHT=y"
```

### Idle baseline

这个版本不启用 PDM/DMIC，`main()` 立即返回，用于测量不运行应用业务时的 SoC 底电流：

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp `
    -d build_idle D:\Project\learning_zephyr_pdm --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=D:/Project/learning_zephyr_pdm/prj_idle.conf"
```

构建后检查：

- `build*/zephyr/zephyr.hex`
- `build*/zephyr/zephyr.dts`
- `build*/zephyr/.config`
- `build*/zephyr/include/generated/zephyr/devicetree_generated.h`

## 烧录和 debug log

```powershell
west flash -d build
```

未接 shield 时，debug log 默认从 nRF54L15DK 的 VCOM1 输出，即 Windows 中通常显示为第二个 COM 口，
波特率为 115200、8N1。打开串口后再复位开发板，以捕获 boot log。

预期输出类似：

```text
nRF54L15DK PDM power demo ready
PDM20 CLK=P1.12 DIN=P1.11, PCM=16 kHz/16-bit
PDM capture starts automatically
PDM capture started (stereo)
Received 100 PCM blocks, last size 1600 bytes
```

Release 固件关闭串口，不应依赖串口输出判断运行状态；使用 PPK2 观察持续录音电流。

## 功耗测量

1. 使用 DK 的 SoC 电源测量路径和 PPK2，按 DK 硬件指南准备电流测量连接。
2. 先烧录 `build_idle` 并复位，记录不运行 PDM/DMIC 应用时的 SoC 底电流。
3. 再烧录 `build_release` 或 `build_release_mono`，复位后 PDM 会自动开始录音。
4. 等待启动瞬态结束，记录持续 PDM 录音期间的平均电流。
5. mono/stereo、PDM 时钟范围、block duration 和电源电压保持一致后再比较不同测试结果。

`build_idle` 才是本工程的 CPU idle 参考镜像；`build_release*` 专门测量持续 PDM 录音功耗。
约 0.7 uA 只是参考基线，实际数值还会受 DK 电源路径、
P1.11/P1.12 外部电平、PPK2 配置和板上漏电影响。不要把 System OFF 电流作为本工程的验收值。
PDM 麦克风自身的供电电流不包含在 SoC 电流中，但外部信号线的电平仍可能影响 SoC 引脚漏电。

## 关键配置

- `CONFIG_PDM_DEMO_ENABLE=n`：关闭应用 PDM 逻辑，`main()` 立即返回。
- `CONFIG_PDM_DEMO_STEREO=y`：默认 stereo，PCM 按 left/right 交错排列。
- `CONFIG_PDM_DEMO_STEREO=n`：mono。
- `CONFIG_PDM_DEMO_MONO_RIGHT=y`：mono 选择 right；未设置时选择 left。
- `clk-frequency-min/max = 795000/805000`：排除 ratio 48，强制 driver 选择 800 kHz / ratio 50。
- `nordic,drive-mode = <NRF_DRIVE_H0H1>`：PDM pin 使用 H0H1 高驱动模式。
- `CONFIG_PDM_DEMO_BLOCK_MS=25`：每个 DMA block 默认 25 ms。
- `CONFIG_PDM_DEMO_BLOCK_COUNT=4`：DMA slab block 数量。

PDM driver 会根据 16 kHz PCM rate 和 Devicetree 中声明的 PDM clock 范围选择可用的 PDM 时钟。
当前 ratio 50 下，32 MHz `PCLK32M` 的实际 PDM clock 为 800 kHz，PCM 采样率精确为 16 kHz。
单麦克风时，只有与外部 L/R 绑定位匹配的 channel 才是有效数据。

## PDM / DMIC 接口调用流程

### 1. Devicetree 描述硬件

当前 overlay 中，`&pdm20` 是 nRF54L15 的实际 PDM 外设，`dmic` 子节点描述连接的数字麦克风：

```dts
&pdm20 {
    status = "okay";

    dmic {
        compatible = "zephyr,pdm-dmic";
        clk-frequency-min = <795000>;
        clk-frequency-max = <805000>;
        channel-left;
        channel-right;
    };
};
```

`pdm-dmic` 不是 I2S 节点。它提供 PDM clock 范围、duty cycle 和左右声道能力，
供 Nordic DMIC driver 进行配置。

### 2. 准备 DMA buffer

DMIC driver 通过 EasyDMA 把转换后的 PCM 写入 RAM。应用使用 `K_MEM_SLAB_DEFINE_STATIC()`
提供 block 内存：

```text
PCM rate × sample width × channel count × block duration
```

本工程默认 16 kHz、16-bit、stereo、25 ms，因此一个 block 是 1600 bytes。

### 3. 构造 `dmic_cfg`

`struct dmic_cfg` 主要包含：

- `io`：PDM clock 允许范围和 duty cycle；
- `streams`：PCM rate、sample width、block size、memory slab；
- `channel`：mono/stereo 和 left/right channel map。

### 4. 配置和启动

```c
dmic_configure(dmic_dev, &cfg);
dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
```

`dmic_configure()` 会检查声道、采样率和位宽，并根据 PCM rate 和允许的 PDM clock
计算 ratio/prescaler。`START` 后，PDM 外设开始输出 CLK，硬件完成滤波、Decimation 和 DMA。

### 5. 读取并释放 PCM block

```c
void *buffer;
size_t size;

dmic_read(dmic_dev, 0, &buffer, &size, 1000);
/* 处理或丢弃 buffer */
k_mem_slab_free(&audio_mem_slab, buffer);
```

`dmic_read()` 成功后，应用拥有这个 buffer；使用完必须归还，否则 DMA slab 会耗尽。
当前 Demo 只统计 block 数量，然后立即释放。

### 6. 持续运行

本 Demo 不调用 `DMIC_TRIGGER_STOP`，因此 PDM 会一直运行。Debug 配置用于观察启动和
block 统计 log，Release 配置关闭 `CONFIG_LOG`、`CONFIG_SERIAL`、console 和 UART backend，
用于测量持续录音功耗。
