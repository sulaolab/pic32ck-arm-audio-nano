# Hardware validation

The supported target is a PIC32CK SG01 Curiosity Nano (EV33V87A) on an AC164162
Curiosity Nano Base for Click boards with one WM8904 mikroBUS Rev5 codec board in
slot 1.

The following baseline behaviour was verified on target hardware:

- WM8904 device ID reads as `0x8904` over I2C.
- The codec supplies 48 kHz FS and 12.288 MHz BCLK for TDM8.
- WM8904 analogue input IN2 changes the TDM data received by the PIC32CK.
- Firmware receive-to-transmit loop-through starts after valid receive and
  transmit DMA activity is confirmed, then operates continuously through the
  codec headphone path.
- The CMSIS-DSP live DRC path operates with four DSP channels, a 35-stage
  DF2T cascade per channel, and the Sonora sample-delay command path enabled.

The `PIC32CK2051SG01064_CMSIS_DRC` configuration is the vendor CMSIS-DSP
reference. `PIC32CK2051SG01064_CMSIS_M33OPT_DRC` preserves the same transport and
CMSIS DF2T API while selecting the project-owned Cortex-M33 kernel.

Use the audio format and hardware topology above when reproducing these results;
other boards, codec revisions, clock sources, or frame formats have not been
qualified by this repository.
