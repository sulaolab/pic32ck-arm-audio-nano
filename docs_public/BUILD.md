# Build and programming

## Prerequisites

- MPLAB X 6.30
- MPLAB XC32 6.00
- ARM::CMSIS pack 6.3.0
- PIC32CK-SG_DFP 1.10.278
- PIC32CK SG01 Curiosity Nano (EV33V87A)

Install the two named packs through MPLAB X Content Manager. `ARM::CMSIS` is
the Cortex-M core-header build dependency recorded by the MPLAB X project; it
is separate from the Arm CMSIS-DSP subset vendored under `src/dsp/cmsis_dsp/`.

Open `pic32ck-arm-audio-nano.X` in MPLAB X and select one of the two permanent
configurations documented in the repository README. The normal reference image
is `PIC32CK2051SG01064_CMSIS_DRC`.

For a command-line build, regenerate MPLAB X makefiles after a fresh clone or a
project edit, then invoke the configuration makefile:

```text
prjMakefilesGenerator.bat pic32ck-arm-audio-nano.X
make -C pic32ck-arm-audio-nano.X -f nbproject/Makefile-PIC32CK2051SG01064_CMSIS_DRC.mk SUBPROJECTS= .build-conf
```

The resulting ROM image is located at:

```text
pic32ck-arm-audio-nano.X/dist/PIC32CK2051SG01064_CMSIS_DRC/production/
```

## Programming

Use the Curiosity Nano on-board debugger (`pkobnano`) with MDB. Keep `program`
and `reset` in the same MDB session so that the new firmware starts immediately:

```text
device PIC32CK2051SG01064
hwtool pkobnano -p 0
program <firmware.elf>
reset
quit
```

The codec headphone output can pop if a running analogue path is reset. Mute or
disconnect downstream audio equipment before programming. After reset, use the
on-board debugger's USB CDC port at 230400 baud, 8 data bits, no parity, and one
stop bit to check startup. See [OPERATION.md](OPERATION.md) for the expected
sequence and normal controls.
