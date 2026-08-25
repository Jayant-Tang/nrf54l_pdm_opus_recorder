# learning_zephyr_pdm

Minimal PDM recording and SoC power-measurement demo for the nRF54L15DK
using the Zephyr DMIC API.

## Hardware

- **Development board**: nRF54L15DK
- **Board target**: `nrf54l15dk/nrf54l15/cpuapp`
- **NCS**: v3.4.0
- **PDM peripheral**: PDM20
- **PCM output**: 16 kHz, 16-bit
- **PDM ratio**: 50
- **PDM GPIO drive**: `H0H1`
- **Runtime mode**: capture starts automatically after boot

### PDM wiring

| nRF54L15DK | PDM microphone |
|---|---|
| P1.12 | CLK |
| P1.11 | DIN / DATA |
| GND | GND |
| External supply | VDD |

For stereo, connect two microphones to the same CLK and DIN signals and
strap their L/R pins to opposite channel selections. For mono, connect one
microphone and select the matching left or right channel configuration.

Do not use P1.11/P1.12 together with an nRF7002 shield. The microphone is
powered externally and its supply current
is not part of the SoC measurement.

## Behavior

1. After boot, the application configures and starts PDM automatically.
2. The main thread continuously calls `dmic_read()` for PCM DMA blocks.
3. This demo does not process or save audio. Each block is immediately
   returned with `k_mem_slab_free()`.
4. Stereo is the default. Mono left and mono right are supported.

Debug firmware logs a block count every 100 blocks. Release firmware disables
logging and is intended for continuous-recording power measurement.

## Build

Initialize the current PowerShell with the nRF Connect SDK toolchain:

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

`prj_release.conf` disables `CONFIG_LOG`, `CONFIG_SERIAL`, the console,
and the UART log backend:

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp `
    -d build_release D:\Project\learning_zephyr_pdm --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=D:/Project/learning_zephyr_pdm/prj_release.conf"
```

### Release mono

Use a separate build directory and add the release configuration:

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp `
    -d build_release_mono D:\Project\learning_zephyr_pdm --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=D:/Project/learning_zephyr_pdm/prj_release.conf" `
       "-DCONFIG_PDM_DEMO_STEREO=n"
```

Mono defaults to the left channel. Select the right channel with:

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp `
    -d build_release_mono_right D:\Project\learning_zephyr_pdm --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=D:/Project/learning_zephyr_pdm/prj_release.conf" `
       "-DCONFIG_PDM_DEMO_STEREO=n" `
       "-DCONFIG_PDM_DEMO_MONO_RIGHT=y"
```

### Idle baseline

This image disables PDM/DMIC and returns immediately from `main()`. Use it
to measure the SoC baseline without application activity:

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp `
    -d build_idle D:\Project\learning_zephyr_pdm --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=D:/Project/learning_zephyr_pdm/prj_idle.conf"
```

After each build, check:

- `build*/zephyr/zephyr.hex`
- `build*/zephyr/zephyr.dts`
- `build*/zephyr/.config`
- `build*/zephyr/include/generated/zephyr/devicetree_generated.h`

## Flashing and debug log

```powershell
west flash -d build
```

Without a shield, debug log is routed to VCOM1 on the nRF54L15DK, usually
the second COM port on Windows, at 115200 8N1. Open the serial port before
resetting the board to capture the boot log.

Expected output:

```text
nRF54L15DK PDM power demo ready
PDM20 CLK=P1.12 DIN=P1.11, PCM=16 kHz/16-bit
PDM capture starts automatically
PDM capture started (stereo)
Received 100 PCM blocks, last size 1600 bytes
```

Release firmware disables serial output. Use the power profiler to observe
continuous-recording current.

## Power measurement

1. Prepare the DK SoC current-measurement path and PPK2 according to the DK
   hardware guide.
2. First flash and reset `build_idle`, then record the SoC baseline with no
   PDM/DMIC application activity.
3. Flash `build_release` or `build_release_mono` and reset. PDM starts
   automatically.
4. Wait for startup transients to settle, then record the average current
   during continuous PDM capture.
5. Keep PCM mode, PDM clock range, block duration, and supply voltage
   unchanged when comparing measurements.

`build_idle` is the CPU-idle reference image; `build_release*` images are
dedicated to continuous PDM capture power. The approximately 0.7 uA value is
only a reference baseline. Results depend on DK power routing, external P1.11/P1.12 levels,
PPK2 settings, and board leakage. Do not use System OFF current as the
acceptance value. Microphone supply current is excluded from the SoC
measurement, but external signal levels can still affect GPIO leakage.

## Key configuration

- `CONFIG_PDM_DEMO_ENABLE=n`: disable application PDM logic and return
  immediately from `main()`.
- `CONFIG_PDM_DEMO_STEREO=y`: default stereo with interleaved left/right PCM.
- `CONFIG_PDM_DEMO_STEREO=n`: mono.
- `CONFIG_PDM_DEMO_MONO_RIGHT=y`: select right in mono; otherwise left.
- `clk-frequency-min/max = 795000/805000`: excludes ratio 48 and forces the
  driver to select 800 kHz / ratio 50.
- `nordic,drive-mode = <NRF_DRIVE_H0H1>`: use H0H1 high-drive mode for PDM pins.
- `CONFIG_PDM_DEMO_BLOCK_MS=25`: default DMA block duration.
- `CONFIG_PDM_DEMO_BLOCK_COUNT=4`: number of DMA slab blocks.

The PDM driver selects an available PDM clock from the 16 kHz PCM rate and
the clock range declared in Devicetree. With ratio 50 and a 32 MHz
`PCLK32M`, the actual PDM clock is 800 kHz and the PCM rate is exactly
16 kHz. With one microphone, only the channel matching the external L/R
strap contains valid data.

## PDM / DMIC API flow

### 1. Describe the hardware in Devicetree

The overlay enables the nRF54L15 PDM peripheral and uses a child node to
describe the connected digital microphone:

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

`zephyr,pdm-dmic` is not an I2S node. It describes the PDM clock range,
duty cycle, and available channels for the Nordic DMIC driver.

### 2. Provide DMA buffers

The DMIC driver uses EasyDMA to write converted PCM data into RAM. The
application provides blocks through `K_MEM_SLAB_DEFINE_STATIC()`:

```text
PCM rate × sample width × channel count × block duration
```

This demo uses 16 kHz, 16-bit, stereo, and 25 ms blocks, so one block is
1600 bytes.

### 3. Build `dmic_cfg`

`struct dmic_cfg` contains:

- `io`: supported PDM clock range and duty cycle;
- `streams`: PCM rate, sample width, block size, and memory slab;
- `channel`: mono/stereo and left/right channel mapping.

### 4. Configure and start

```c
dmic_configure(dmic_dev, &cfg);
dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
```

`dmic_configure()` validates the requested rate, width, and channel map,
then calculates the PDM ratio and prescaler. After `START`, the PDM
peripheral generates CLK, performs filtering and decimation, and transfers
PCM blocks through EasyDMA.

### 5. Read and release PCM blocks

```c
void *buffer;
size_t size;

dmic_read(dmic_dev, 0, &buffer, &size, 1000);
/* Process or discard buffer. */
k_mem_slab_free(&audio_mem_slab, buffer);
```

After a successful `dmic_read()`, the application owns the buffer and must
return it to the slab. Otherwise, the DMA buffer pool eventually runs out.
This demo only counts blocks and immediately releases them.

### 6. Continuous capture

This demo does not call `DMIC_TRIGGER_STOP`, so PDM runs continuously.
Debug configuration is for observing startup and block-count logs. Release
configuration disables `CONFIG_LOG`, `CONFIG_SERIAL`, the console, and the
UART backend for power measurement.
