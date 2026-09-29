# Implementation and verification sources

The two supported MPLAB X configurations build the same project source list.
They differ only in the DF2T implementation selected by `src/app_config.h`:
the CMSIS reference uses the vendored Arm implementation, while the M33OPT
configuration uses the project implementation through the same CMSIS API.

## Audio path sources

`src/audio/`, `src/drivers/`, and `src/dsp/sonora_drc_path.*` implement the
supported WM8904-to-WM8904 live path. Some identifiers retain the `sonora_`
prefix because the compatibility interface was preserved during the port; they
are part of this tree and do not require another checkout.

`src/dsp/cmsis_dsp/` is the bundled third-party source component whose license
and provenance are confirmed and recorded in this tree. It is the pinned
CMSIS-DSP 1.17.0 subset described in
[`VENDOR.md`](../src/dsp/cmsis_dsp/VENDOR.md) and remains under Apache-2.0.

## Included verification and measurement code

The MPLAB X project also lists several verification translation units. They
remain in the public source because excluding them would make the project file
and the two supported configurations describe a different source set.

| Files | Purpose in this source tree | Supported live-image behaviour |
| --- | --- | --- |
| `dsp_bench.*`, `dsp_iir.*`, `dsp_tier2.*`, `dsp_signal.*` | Optional kernel-only and DRC-chain measurement paths. | Disabled by `APP_ENABLE_DSP_BENCH=0`. |
| `dsp_coeffs*`, `dsp_vectors*` | Coefficients and known-answer fixtures used by the optional checks. | Disabled with the optional benchmark path. |
| `dsp_cycles.*` | Cycle-counter abstraction used for optional measurements and live DRC telemetry. | Included by the live DRC path. |
| `dsp_dwt_diag.*` | Optional diagnostic counter reporting. | Disabled unless explicitly enabled. |

The source labels **Tier 1** and **Tier 2** identify measurement scope, not
extra product configurations: Tier 1 is the DF2T kernel alone, and Tier 2 adds
the codec-sample conversion and channel-expansion chain. The supported live
images add the actual TDM/DMA transport. No historical benchmark result is
implied by retaining these files.

Generated coefficient and vector source is included so that a clone builds
without a generator. The generator and maintainer host-test tools are not part
of this public firmware source set; changing generated fixtures requires an
equivalent independently validated generation process.
