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
3. By default each block is encoded as one Opus frame in real time with
   statistics logging. With `CONFIG_PDM_TEST_ONLY=y` the block is released
   via `k_mem_slab_free()` right after the read, without encoding.
4. Stereo is the default. Mono left and mono right are supported.

Debug firmware logs a block count every 100 blocks. Release firmware disables
logging and is intended for continuous-recording power measurement.

## Clone and submodules

The Opus codec is integrated as a git submodule (`lib/opus`, upstream xiph/opus,
currently v1.5.2). Initialize it after cloning:

```powershell
git submodule update --init
```

To upgrade Opus:

```powershell
cd lib/opus
git fetch --tags
git checkout v1.x.y    # target release tag
cd ../..
git add lib/opus       # commit the new submodule pointer
```

The Zephyr module glue lives in `modules/opus/` (maintained in this repo) and is
registered via `ZEPHYR_EXTRA_MODULES` in `CMakeLists.txt`. Sources are collected
with `file(GLOB)` per directory (excluding demo/tool programs), so minor upstream
upgrades usually need no glue changes. If an upgrade adds a new top-level source
directory (e.g. `dnn/` in 1.5), revisit `modules/opus/CMakeLists.txt`.

### Opus encoding path

- Opus encoding is the **default path**: each DMA block is encoded as one
  Opus frame in real time; every 100 blocks a statistics line is logged
  (average bytes, max per-frame encode time, error count).
- `CONFIG_PDM_TEST_ONLY=y`: plain PDM power-measurement switch (default n);
  blocks are released via `k_mem_slab_free()` right after `dmic_read()`,
  skipping the encoder. Release images encode by default; overlay
  `-DCONFIG_PDM_TEST_ONLY=y` for a plain PDM power baseline.
- Fixed-point only build (no float option): measured on a 128 MHz
  Cortex-M33, the float build cannot reach real time even at complexity 0
  (~31 ms per 20 ms stereo frame), so only fixed-point is kept. With
  fixed-point + EDSP at complexity 0, steady state is ~4.7 ms per frame
  (~23% CPU).
- The fixed-point build enables the ARMv5E EDSP inline-asm optimizations
  (`OPUS_ARM_INLINE_ASM` + `OPUS_ARM_INLINE_EDSP`; the ARMv8-M DSP
  extension of the M33 supports these instructions and needs no Zephyr-side
  configuration). The M33 has no NEON, so opus's NEON intrinsics do not
  apply.
- `CONFIG_PDM_DEMO_OPUS_COMPLEXITY` (default 0): Opus complexity knob.
- `CONFIG_PDM_DEMO_SAMPLE_RATE` (default 16000): PCM sample rate shared by
  the PDM capture and the Opus encoder. Opus accepts 8/12/16/24/48 kHz
  (enforced by a `BUILD_ASSERT`); when changing it, make sure the PDM clock
  range in the board overlay still allows an integer decimation ratio.
- Legal Opus frame sizes: `CONFIG_PDM_DEMO_BLOCK_MS` is a Kconfig choice
  offering only 5/10/20/40/60 ms (default 20), so illegal frame sizes are
  impossible at configuration time.
- Heap and main stack are sized for Opus (128 KB / 64 KB) via Kconfig
  defaults. Note that `PDM_TEST_ONLY` images also create the encoder (they
  just never call the encode path), so their memory footprint matches the
  encoding image.
- `build_release*` measures PDM + Opus encoding power by default; build
  with `-DCONFIG_PDM_TEST_ONLY=y` for the plain PDM baseline.

## Build

All commands assume the current directory is the project root:

### Debug stereo (default, with Opus encoding)

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -d build . --no-sysbuild
```

### Release stereo

`prj_release.conf` disables `CONFIG_LOG`, `CONFIG_SERIAL`, the console,
and the UART log backend:

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -d build_release . --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=prj_release.conf"
```

### Release mono

Use a separate build directory and add the mono (left) configuration:

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -d build_release_mono . --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=prj_release.conf" `
       "-DCONFIG_PDM_DEMO_MONO_LEFT=y"
```

Use `"-DCONFIG_PDM_DEMO_MONO_RIGHT=y"` for the right channel.

### Plain PDM power baseline (no encoding)

```powershell
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -d build_test_only . --no-sysbuild `
    -- "-DEXTRA_CONF_FILE=prj_release.conf" `
       "-DCONFIG_PDM_TEST_ONLY=y"
```

The nRF54LM20DK is also supported: use board target
`nrf54lm20dk/nrf54lm20b/cpuapp` (matching files exist under `boards/`).

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
nrf54l15dk PDM power demo ready
PDM20 CLK=P1.12 DIN=P1.11, PCM=16000 Hz/16-bit
PDM capture starts automatically
Opus encoder ready: state 43308 bytes, frame 320 samples/ch (20 ms), complexity 3
PDM capture started (stereo)
Blocks 100, opus avg 10 B/frame, max enc 23952 us, err 0
```

Release firmware disables serial output. Use the power profiler to observe
continuous-recording current.

## Power measurement

1. Prepare the DK SoC current-measurement path and PPK2 according to the DK
   hardware guide.
2. Flash `build_release` or `build_release_mono` and reset. PDM starts
   automatically.
3. Wait for startup transients to settle, then record the average current
   during continuous PDM capture.
4. Keep PCM mode, PDM clock range, block duration, and supply voltage
   unchanged when comparing measurements.

`build_release*` images measure continuous PDM capture + Opus encoding
power; build with `-DCONFIG_PDM_TEST_ONLY=y` for the plain PDM capture
baseline. Results depend on DK power routing, external P1.11/P1.12 levels,
PPK2 settings, and board leakage. Microphone supply current is excluded
from the SoC measurement, but external signal levels can still affect GPIO
leakage.

## Key configuration

- Channel selection is a 3-way choice: `CONFIG_PDM_DEMO_STEREO` (default,
  interleaved left/right), `CONFIG_PDM_DEMO_MONO_LEFT`,
  `CONFIG_PDM_DEMO_MONO_RIGHT`.
- `clk-frequency-min/max = 795000/805000`: excludes ratio 48 and forces the
  driver to select 800 kHz / ratio 50.
- `nordic,drive-mode = <NRF_DRIVE_H0H1>`: use H0H1 high-drive mode for PDM pins.
- `CONFIG_PDM_DEMO_BLOCK_MS`: DMA block duration; a Kconfig choice allows
  only 5/10/20/40/60 ms (legal Opus frame sizes), default 20 ms.
- `CONFIG_PDM_DEMO_BLOCK_COUNT`: number of DMA slab blocks, default 4,
  range 4-8 (the minimum matches the driver `queue-size = <4>` in the
  board overlays; fewer would starve the DMA queue at startup).

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

This demo uses 16 kHz, 16-bit, stereo, and 20 ms blocks, so one block is
1280 bytes.

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

### 5. Read, encode, and release PCM blocks

```c
void *buffer;
size_t size;

dmic_read(dmic_dev, 0, &buffer, &size, 1000);
opus_encode_block(buffer);   /* no-op under CONFIG_PDM_TEST_ONLY */
k_mem_slab_free(&audio_mem_slab, buffer);
```

After a successful `dmic_read()`, the application owns the buffer and must
return it to the slab. Otherwise, the DMA buffer pool eventually runs out.

On the default path, each block is exactly one Opus frame (16 kHz, 20 ms,
320 samples per channel) and is encoded in real time by
`opus_encode_block()` via `opus_encode()`. Frame-size legality is enforced
by a `BUILD_ASSERT` (block duration must be 5/10/20/40/60 ms). Encode
timing is measured with the cycle counter: the first 10 frames are logged
individually, then every 100 blocks a line with average bytes, max
per-frame encode time, and error count is printed. Encoded packets are
currently only counted, not stored.

With `CONFIG_PDM_TEST_ONLY=y`, `opus_encode_block()` is a no-op and blocks
are released right after the read, for plain PDM power measurement.

### 6. Continuous capture

This demo does not call `DMIC_TRIGGER_STOP`, so PDM runs continuously.
Debug configuration is for observing startup and block-count logs. Release
configuration disables `CONFIG_LOG`, `CONFIG_SERIAL`, the console, and the
UART backend for power measurement.
