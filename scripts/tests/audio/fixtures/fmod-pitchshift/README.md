# FMOD pitch shifter references

Recorded from Geometry Dash's own `fmod.dll` to check the plugin's port of FMOD's
`FMOD_DSP_TYPE_PITCHSHIFT` against it. Mono at 48 kHz, left channel; 16-bit PCM except the noise
input, which stays 32-bit float so the vocoder's chaotic phase estimates start from exactly the
samples FMOD processed.

| File | What it is |
|---|---|
| `noise-in.wav` | Two seconds of deterministic noise, the input FMOD processed (32-bit float) |
| `noise-fft1024-p75.wav` | Output for FFT 1024, pitch 0.75, frames 24000 to 72000 |
| `noise-fft4096-p75.wav` | Output for FFT 4096, pitch 0.75, frames 24000 to 72000 |
| `noise-fft4096-p100.wav` | Output for FFT 4096, pitch 1, frames 24000 to 72000 |
| `sine-in.wav` | Two seconds of a 0.3 amplitude 440 Hz sine, the input FMOD processed |
| `sine-fft4096-p75.wav` | Output for FFT 4096, pitch 0.75, frames 24000 to 72000 |
| `burst-gap-in.wav` | 0.25 s of 440 Hz, 0.25 s of digital silence, 0.25 s of 660 Hz, 0.5 s of silence |
| `burst-gap-out.wav` | Output for FFT 4096, pitch 0.75, all frames |

Generated with `tools/qa/fmod-pitchshift-reference.cpp`, which plays the input through a
`PITCHSHIFT` stage and records the master output. The tool sets FMOD's DSP block size to 256,
so the plugin is fed the same 256-frame calls; its gate uses `gateFrames = 256` to apply the same
per-block silence rule.

Command (Windows, against the local Steam install):

```powershell
cmake --build build-windows/qa --config Release --target fmod-pitchshift-reference
build-windows/qa/Release/fmod-pitchshift-reference.exe "C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash" artifacts/fmod-reference-v2
```

`fmod.dll` is FMOD 2.02.31 (build 157574), SHA-256
`223f0e3fd8af19dbab92b4cef1a77580aef3d0a510b47a76561833e31348b720`. The comparison window is
frames 24000 to 72000, the window the shifter was measured in; the gap file is compared whole.

The bus-separation figure quoted in the README comes from the same run: a single stage on the
sum of the noise and the sine against the separate stage outputs correlates 0.995 at 48 kHz over
frames 24000 to 72000.

SHA-256 of the files:

| File | SHA-256 |
|---|---|
| `burst-gap-in.wav` | `b65c7e0dc254d3283f92d6825ce7a26e26a5547d863d3566258d4130f6f5606e` |
| `burst-gap-out.wav` | `276cde13b940996d9a9ebdd697d9ababca4db9592d6740dc1f39c730035c4b6c` |
| `noise-fft1024-p75.wav` | `9408a6c32c19962321f19e044dc0aacca046f4a0914e40e7a27fcb331c522f8a` |
| `noise-fft4096-p100.wav` | `9aa4481425e911fa805de271d3cd5670ceee909f50bd59b0b5b80281ddf996d7` |
| `noise-fft4096-p75.wav` | `817d4caa0d65f2a4e9a355084f497b66f366977901af49ebad5a1810810c8849` |
| `noise-in.wav` | `f336ac07ebeb5d5b58535f85451a1c59e791286139c7a1942c3e6fd8849013d6` |
| `sine-fft4096-p75.wav` | `56689343b0fb631f9281b4af7635ba986716dac9cd20130f33245c8558e10ade` |
| `sine-in.wav` | `89897eeb65f620d3b4ec3a95c45fc9089eb02ff7cf7788e6f6ca56445b34bd23` |
