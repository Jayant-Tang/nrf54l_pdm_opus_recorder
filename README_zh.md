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
3. 默认每个 block 作为一个 Opus 帧实时编码并输出统计；
   `CONFIG_PDM_TEST_ONLY=y` 时读取后直接 `k_mem_slab_free()` 释放，不编码。
4. 默认是 stereo；mono 可选择 left 或 right。

Debug 固件每收到 100 个 block 输出一次统计 log；Release 固件关闭 log，适合直接测量持续录音功耗。

## 克隆与子模块

本工程通过 git submodule 集成 Opus 编解码器（`lib/opus`，xiph/opus 官方仓库，当前 v1.5.2）。
git clone 后必须先初始化子模块：

```powershell
git submodule update --init
```

如果要升级 Opus 版本：

```powershell
cd lib/opus
git fetch --tags
git checkout v1.x.y    # 目标版本 tag
cd ../..
git add lib/opus       # 提交新的 submodule 指针
```

Zephyr module 胶水层在 `modules/opus/`，通过 `CMakeLists.txt` 中的
`ZEPHYR_EXTRA_MODULES` 注册。源文件用 `file(GLOB)` 按目录收集（排除 demo/工具程序），
上游小版本升级一般无需改动胶水层；若上游新增顶层源码目录（如 1.5 引入的 `dnn/`），
需回看 `modules/opus/CMakeLists.txt` 决定是否纳入。

### Opus 编码通路

- Opus 编码是**默认通路**：每个 DMA block 作为一个 Opus 帧实时编码，
  每 100 块输出一次统计（平均字节数、最大单帧编码耗时、错误数）。
- `CONFIG_PDM_TEST_ONLY=y`：纯 PDM 功耗测量开关（默认 n），`dmic_read()`
  后直接 `k_mem_slab_free()` 释放，跳过编码。release 固件默认也执行编码；
  需要纯 PDM 功耗基线时手动叠加：`-DCONFIG_PDM_TEST_ONLY=y`。
- 定点构建（无浮点选项）：实测 128 MHz Cortex-M33 上 float 即使
  complexity 0 也要 ~31 ms/20 ms 帧（无法实时），故只保留定点；
  定点 + EDSP、complexity 0 稳态约 4.7 ms/帧（~23% CPU）。
- 定点构建启用 ARMv5E EDSP 内联汇编优化（`OPUS_ARM_INLINE_ASM` +
  `OPUS_ARM_INLINE_EDSP`，M33 的 ARMv8-M DSP 扩展支持这些指令，无需
  Zephyr 侧额外配置）。M33 没有 NEON，opus 的 NEON intrinsics 不适用。
- `CONFIG_PDM_DEMO_OPUS_COMPLEXITY`（默认 0）：Opus 复杂度，越大越慢。
- `CONFIG_PDM_DEMO_SAMPLE_RATE`（默认 16000）：PCM 采样率，PDM 采集和
  Opus 编码共用。Kconfig choice 只提供 Opus 合法的 8/12/16/24/48 kHz；
  改动时需确认 overlay 里 PDM 时钟范围能整除出目标采样率。
- Opus 合法帧长限制：`CONFIG_PDM_DEMO_BLOCK_MS` 是 Kconfig choice，
  只有 5/10/20/40/60 ms 可选（默认 20），非法值在配置期就不存在。
- heap 和 main stack 通过 Kconfig 默认值设为 128 KB / 64 KB；
  注意 `PDM_TEST_ONLY` 固件也会创建编码器（只是不调用编码），
  内存占用与编码固件一致。
- `build_release*` 默认测量 PDM + Opus 编码功耗；叠加
  `-DCONFIG_PDM_TEST_ONLY=y` 才是纯 PDM 基线。

## 构建

以下命令均假设当前目录是工程根目录：

### Debug stereo（默认，含 Opus 编码）

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -d build . --no-sysbuild
```

### Release stereo

`prj_release.conf` 会关闭 `CONFIG_LOG`、`CONFIG_SERIAL`、console 和 UART backend：

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -d build_release . --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=prj_release.conf"
```

### Release mono

使用新的 build directory，并叠加 mono 配置（left）：

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -d build_release_mono . --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=prj_release.conf" `
       "-DCONFIG_PDM_DEMO_MONO_LEFT=y"
```

选择 right 用 `"-DCONFIG_PDM_DEMO_MONO_RIGHT=y"`。

### 纯 PDM 功耗测试（不编码）

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -d build_test_only . --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=prj_release.conf" `
       "-DCONFIG_PDM_TEST_ONLY=y"
```

也支持 nRF54LM20DK：把 board target 换成 `nrf54lm20dk/nrf54lm20b/cpuapp` 即可，
`boards/` 下已有对应配置。

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
nrf54l15dk PDM power demo ready
PDM20 CLK=P1.12 DIN=P1.11, PCM=16000 Hz/16-bit
PDM capture starts automatically
Opus encoder ready: state 43308 bytes, frame 320 samples/ch (20 ms), complexity 3
PDM capture started (stereo)
Blocks 100, opus avg 10 B/frame, max enc 23952 us, err 0
```

Release 固件关闭串口，不应依赖串口输出判断运行状态；使用 PPK2 观察持续录音电流。

## 功耗测量

1. 使用 DK 的 SoC 电源测量路径和 PPK2，按 DK 硬件指南准备电流测量连接。
2. 烧录 `build_release` 或 `build_release_mono`，复位后 PDM 会自动开始录音。
3. 等待启动瞬态结束，记录持续 PDM 录音期间的平均电流。
4. mono/stereo、PDM 时钟范围、block duration 和电源电压保持一致后再比较不同测试结果。

`build_release*` 测量持续 PDM 录音 + Opus 编码功耗；叠加
`-DCONFIG_PDM_TEST_ONLY=y` 构建的固件才是纯 PDM 录音基线。
实际数值会受 DK 电源路径、P1.11/P1.12 外部电平、PPK2 配置和板上漏电影响。
PDM 麦克风自身的供电电流不包含在 SoC 电流中，但外部信号线的电平仍可能影响 SoC 引脚漏电。

## 关键配置

- 声道是三选一 choice：`CONFIG_PDM_DEMO_STEREO`（默认，left/right 交错）、
  `CONFIG_PDM_DEMO_MONO_LEFT`、`CONFIG_PDM_DEMO_MONO_RIGHT`。
- `clk-frequency-min/max = 795000/805000`：排除 ratio 48，强制 driver 选择 800 kHz / ratio 50。
- `nordic,drive-mode = <NRF_DRIVE_H0H1>`：PDM pin 使用 H0H1 高驱动模式。
- `CONFIG_PDM_DEMO_BLOCK_MS`：每个 DMA block 的时长，Kconfig choice 只允许
  5/10/20/40/60 ms（合法 Opus 帧长），默认 20 ms。
- `CONFIG_PDM_DEMO_BLOCK_COUNT`：DMA slab block 数量，默认 4，范围 4-8
  （下限对应 overlay 里驱动的 `queue-size = <4>`，再少会在启动时欠载）。

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

本工程默认 16 kHz、16-bit、stereo、20 ms，因此一个 block 是 1280 bytes。

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

### 5. 读取、编码并释放 PCM block

```c
void *buffer;
size_t size;

dmic_read(dmic_dev, 0, &buffer, &size, 1000);
opus_encode_block(buffer);   /* CONFIG_PDM_TEST_ONLY 时为空操作 */
k_mem_slab_free(&audio_mem_slab, buffer);
```

`dmic_read()` 成功后，应用拥有这个 buffer；使用完必须归还，否则 DMA slab 会耗尽。

默认通路下，每个 block 正好是一个 Opus 帧（16 kHz、20 ms、320 样本/声道），
由 `opus_encode_block()` 调 `opus_encode()` 实时编码。帧长合法性由
`BUILD_ASSERT` 保证（block 时长只能是 5/10/20/40/60 ms）。编码耗时用
cycle counter 统计：前 10 帧逐帧打印，之后每 100 块输出一次平均字节数、
最大单帧耗时和错误数。编码结果当前只统计、不保存。

`CONFIG_PDM_TEST_ONLY=y` 时 `opus_encode_block()` 为空操作，block 读取后
直接释放，用于纯 PDM 功耗测量。

### 6. 持续运行

本 Demo 不调用 `DMIC_TRIGGER_STOP`，因此 PDM 会一直运行。Debug 配置用于观察启动和
block 统计 log，Release 配置关闭 `CONFIG_LOG`、`CONFIG_SERIAL`、console 和 UART backend，
用于测量持续录音功耗。
