# Operation

## Audio and console connections

Install the WM8904 mikroBUS Rev5 board in slot 1 of the Curiosity Nano Base for
Click boards. The supported audio input is WM8904 analogue input IN2. Connect
the monitored output to the codec headphone connector.

The firmware console is available through the Curiosity Nano on-board
debugger's USB CDC port. Configure the terminal for 230400 baud, 8 data bits,
no parity, and one stop bit.

Keep the headphone output muted, or disconnect downstream audio equipment,
while programming or resetting the board.

## Normal startup

Program either supported configuration and reset the board. The firmware first
configures the WM8904 as the audio clock source, then starts the PIC32CK TDM8
transport. Headphone output remains muted until valid receive and transmit DMA
activity has been observed.

A successful startup reports `PASS` for codec and transport initialization. The
live path is:

```text
WM8904 IN2 -> TDM8 receive -> four-channel DF2T processing
           -> TDM8 transmit -> WM8904 headphone output
```

`PIC32CK2051SG01064_CMSIS_DRC` uses the Arm CMSIS-DSP DF2T implementation.
`PIC32CK2051SG01064_CMSIS_M33OPT_DRC` uses the project-owned Cortex-M33 kernel
through the same CMSIS API.

## Normal console controls

The following commands are available in both supported configurations:

| Command | Action |
| --- | --- |
| `h` | Print the console help. |
| `s` | Print codec, transport, DMA, and DSP status. |
| `m` | Mute the headphone output and drain transmit audio to silence. |
| `l` | Restart receive-to-transmit processing; output unmutes after valid DMA activity is confirmed. |
| `p` | Stop the audio transport without stopping the codec clock. |
| `o` | Start the audio transport while leaving the codec clock unchanged. |
| `*cf00` | Select normal IIR processing. |
| `*cf01` | Bypass IIR processing. |

The status display includes receive and transmit block counts, transport error
counters, DSP block count, and deadline misses. A DWT timing line may report
`UNAVAILABLE` when the hardware cycle counter is unavailable; that does not by
itself mean that audio transport failed.
