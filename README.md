# PIC32CK ARM Audio Nano

Firmware for a PIC32CK2051SG01064 Curiosity Nano audio board with a WM8904
mikroBUS codec. It is a compact, hardware-validated Cortex-M33 audio transport
and DF2T IIR-cascade reference built around the PIC32CK SG01 Curiosity Nano,
the Curiosity Nano Base for Click boards, and one WM8904 mikroBUS Rev5 board.

## Supported hardware

| Item | Detail |
| --- | --- |
| MCU | PIC32CK2051SG01064, Cortex-M33 with single-precision FPU |
| Board | PIC32CK SG01 Curiosity Nano (EV33V87A) |
| Codec carrier | Curiosity Nano Base for Click boards (AC164162), mikroBUS slot 1 |
| Codec | WM8904 mikroBUS Rev5 |
| Audio format | 48 kHz TDM8, 8 slots x 32 bits |
| Clock owner | WM8904 codec master; BCLK = 12.288 MHz |
| Toolchain | MPLAB X 6.30, XC32 6.00, ARM::CMSIS 6.3.0, PIC32CK-SG_DFP 1.10.278 |

The audio path uses WM8904 analogue input IN2. The WM8904 clock and codec
configuration are part of the firmware; no external audio-clock generator is
required.

## Configurations

The MPLAB X project intentionally has two permanent configurations:

| Configuration | Purpose |
| --- | --- |
| `PIC32CK2051SG01064_CMSIS_DRC` | The hardware-qualified live-audio reference: vendor CMSIS-DSP DF2T, four DSP channels, 35 stages per channel, and Sonora sample-delay controls. |
| `PIC32CK2051SG01064_CMSIS_M33OPT_DRC` | The same live transport and CMSIS DF2T API using the project-owned Cortex-M33 optimized kernel. |

These are the supported build entry points. Their profile definitions are kept
in `src/app_config.h` so the selected MPLAB X configuration determines the
complete firmware feature set.

## Build

Open `pic32ck-arm-audio-nano.X` in MPLAB X, select one of the two configurations
above, and build. From a command prompt where the MPLAB X GNU utilities and XC32
are available, regenerate the project makefiles once after cloning or editing the
project, then build the desired configuration:

```text
prjMakefilesGenerator.bat pic32ck-arm-audio-nano.X
make -C pic32ck-arm-audio-nano.X -f nbproject/Makefile-PIC32CK2051SG01064_CMSIS_DRC.mk SUBPROJECTS= .build-conf
```

The ROM image is written below
`pic32ck-arm-audio-nano.X/dist/<configuration>/production/`.

For target setup, flashing, normal operation, and the verified operating
envelope, see:

- [Build and programming](docs_public/BUILD.md)
- [Implementation and verification sources](docs_public/IMPLEMENTATION.md)
- [Operation](docs_public/OPERATION.md)
- [Hardware validation](docs_public/VALIDATION.md)
- [Documentation index](docs_public/README.md)

## License and third-party material

Original SulaoLab contributions are licensed under [MIT No Attribution](LICENSE)
(MIT-0). The vendored Arm CMSIS-DSP files remain under Apache-2.0; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the license included in
`src/dsp/cmsis_dsp/`.
