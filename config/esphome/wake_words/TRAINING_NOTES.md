# "Hey TARS" Wake Word — Training Notes (Phase 0A)

**Date:** 2026-05-26
**Model:** `hey_tars.tflite` (microWakeWord, INT8-quantized streaming, ESP32-S3)
**Target device:** FutureProofHomes satellite1 (ESP32-S3 w/ PSRAM), ESPHome 2025.12.7
**Trained on:** RTX 5080 Gaming_PC (192.168.51.54), Windows 11, via Docker

---

## Result (stop gate)

`hey_tars.tflite` — 60,968 bytes. Verified valid INT8 streaming model:
- Input `[1, 3, 40]` int8 (3 feature frames × 40 mel bins), output `[1, 1]` uint8 probability.

Held-out evaluation (`hey_tars_roc_metrics.txt`), quantized streaming model:

| probability cutoff | false-reject rate | false-accepts/hour |
|--------------------|-------------------|--------------------|
| 0.32 | 0.0000 | 0.000 |
| 0.16 | 0.0000 | 0.937 |
| 0.10 | 0.0000 | 1.500 |
| 0.07 | 0.0000 | 2.000 |

100% detection (0% FRR) with **0 false-accepts/hour at cutoff 0.32** — meets the
Phase 0A target (>95% detection, <1% false-positive) on the held-out test set.

> **Honesty caveat:** the test/validation sets are held-out splits of the *same
> Piper-TTS-generated* positive samples plus the kahrendt ambient negative set.
> The upstream tooling explicitly warns that benchmarking on TTS samples generated
> the same way as training "can make the model benchmark better than it performs in
> real-world use." Treat these numbers as an upper bound. Real-world performance on
> Chris's actual voice in the actual room should be validated on-device (Phase 3C),
> and `probability_cutoff` tuned from there.

---

## Toolchain

- **Framework:** [FutureProofHomes/microWakeWord](https://github.com/FutureProofHomes/microWakeWord) (fork of OHF-Voice/kahrendt microWakeWord). Chosen because satellite1 is FutureProofHomes hardware.
- **Positive samples:** [rhasspy/piper-sample-generator](https://github.com/rhasspy/piper-sample-generator) **@ tag v2.0.0** (the version matching the `en_US-libritts_r-medium.pt` voice and the `piper-phonemize` API; current `main` restructured into a package and broke the recipe).
- **Voice model:** `en_US-libritts_r-medium.pt` (multi-speaker VITS, 204 MB).
- **Negative datasets (mandatory, precomputed mmap features):** `speech`, `dinner_party`, `no_speech`, `dinner_party_eval` from [huggingface.co/datasets/kahrendt/microwakeword](https://huggingface.co/datasets/kahrendt/microwakeword).
- **Python:** 3.10 (microWakeWord requires `>=3.10,<3.11`). **TensorFlow:** 2.17.1.

Everything runs in two Docker images (`Dockerfile.piper`, `Dockerfile.mww`) — see the
training workspace at `C:\AI\hey-tars-training\` (not committed; scratch).

## Pipeline

1. Generate 1000 "Hey TARS" samples (Piper, multi-speaker, varied length/noise scales).
2. Augment positives (EQ, distortion, pitch-shift, band-stop, color-noise) → `pymicro-features` 40-bin spectrograms → train/val/test mmaps.
3. Train `mixednet` (medium preset, 10,000 steps, batch 128, negative_class_weight 20).
4. Quantize to INT8 streaming TFLite; evaluate ROC on held-out test + ambient.

Model: 30,225 params (~118 KB float; 61 KB INT8 tflite).

---

## Implementation decisions / deviations (documented)

1. **GPU not used — trained on CPU.** Docker→CUDA passthrough works, but the RTX 5080
   is Blackwell (compute capability **sm_120**), which needs CUDA 12.8+. The only
   TensorFlow versions compatible with the **required Python 3.10** (TF 2.16–2.18)
   bundle CUDA ≤12.5 and have no sm_120 kernels, so TF cannot use this GPU. CPU
   training of this tiny model took ~17 min; **model validity is identical** to a
   GPU-trained one.

2. **RIR / reverb augmentation disabled.** `audiomentations` 0.43.1's
   `ApplyImpulseResponse` segfaults (SIGSEGV) non-deterministically on the MIT RIR set
   with this native stack, and `audiomentations==0.33.0` (the version the recipe
   targeted) is API-incompatible with the fork's `Augmentation` class. All other
   augmentations and the `pymicro-features` frontend are stable. Reverb is a robustness
   enhancement, not a correctness requirement. **Future improvement:** restore reverb
   augmentation once a compatible, non-crashing audiomentations RIR path is found
   (the MIT RIRs are already downloaded to the workspace).

3. **Background-noise augmentation (AudioSet/FMA) skipped.** Those are multi-GB
   downloads + conversions; deferred for a first model. `mit_rirs` was fetched but is
   unused (see #2). Can be added in a retrain to further harden false-positive rate.

4. **Manifest schema corrected** — see `hey_tars.json` note below.

## Upstream bugs patched (version drift in the FPH fork vs current libraries)

The FPH fork's code was written against older library versions; these were patched in
the (vendored, non-committed) training copy:

- `datasets` pinned `<3` — datasets 3.x routes `Audio` decoding through `torchcodec`/`torch` (not in the TF image); `<3` decodes via `soundfile`.
- `audio_utils.py` — `MicroFrontend.ProcessSamples()` → `process_samples()` (PyPI `pymicro-features` renamed it).
- `train.py` — `result["fp"].numpy()` etc. → `np.asarray(...)`; TF 2.17 returns metric results as numpy arrays, not EagerTensors.
- Feature generation must run **before** any TF runtime init (`tf.config.*`) — TF's thread pools racing with the pymicro/audiomentations C code caused a non-deterministic SIGSEGV. Also pinned BLAS to 1 thread + disabled oneDNN during feature gen.

---

## `hey_tars.json` manifest

Written to the **ESPHome micro_wake_word v2** schema (verified against
esphome.io/components/micro_wake_word and the okay_nabu v2 manifest), which differs
from the literal JSON in the build plan in two ways that would otherwise prevent the
model from loading on satellite1:

- field is **`sliding_window_size`** (not `sliding_window_average_size`);
- v2 requires top-level **`version: 2`**, `author`, `trained_languages`.

All build-plan *values* were preserved: `probability_cutoff` 0.75, `feature_step_size`
10, `tensor_arena_size` 65536, `minimum_esphome_version` "2025.12.7".

> **Tuning note:** the ROC data shows this model operates cleanly (0% FRR, 0 faph)
> down to cutoff **0.32**. The manifest's 0.75 is conservative (favors fewer false
> positives). If "Hey TARS" feels hard to trigger on-device, lower `probability_cutoff`
> toward 0.4–0.5.
