# Vendored CMSIS-DSP

The files in this directory are an unmodified subset of Arm CMSIS-DSP 1.17.0.
Keep the vendored files byte-for-byte unchanged so the firmware continues to
identify a standard upstream release rather than a local variant.

| | |
|---|---|
| Package | `ARM.CMSIS-DSP` |
| Version | **1.17.0** |
| Origin | Arm CMSIS-DSP 1.17.0, obtained through the Microchip package distribution |
| Licence | Apache-2.0, see `LICENSE` |
| Copied | 2026-09-16 |

## Why a copy instead of the pack path

The installed package cache is machine-local. Referencing it directly would make
the build depend on an unspecified local package version. The version is pinned
here in the tree, so the source and the reported result identify the same
library release.

## What was copied

```
Include/            all public headers        (verbatim)
PrivateInclude/     all private headers       (verbatim)
LICENSE                                       (verbatim)
Source/FilteringFunctions/
    arm_biquad_cascade_df2T_f32.c             (verbatim)
    arm_biquad_cascade_df2T_init_f32.c        (verbatim)
```

Only the two source files required by this firmware are included. The remaining
CMSIS-DSP source modules are not part of this repository.

## How it is compiled

Never directly. `../dsp_cmsis_dsp_unity.c` includes the two `.c` files in one
translation unit, so:

- the vendor files remain byte-verbatim,
- they inherit exactly the firmware's compile options,
- the vendor baseline remains separately identifiable from the project-owned
  M33 implementation.

## Loop-unroll selection

`ARM_MATH_LOOPUNROLL` is not defined by the vendored headers. The CMSIS DRC
reference configuration enables it through `APP_ENABLE_CMSIS_LOOPUNROLL`. The
M33-optimized configuration selects the project-owned implementation through
the same CMSIS DF2T API without modifying these vendor files.
