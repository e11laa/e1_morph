# E1Morph

THE LATEST VST3 BUILD DOWNLOAD : https://www.mediafire.com/file/fjmhjlu7ojw4ibs/E1Morph.vst3/file

E1Morph is a JUCE-based spectral morphing audio plugin (VST3 + Standalone).
It morphs a source input toward a sidechain target using 1D Optimal Transport
(Sinkhorn) in the frequency domain.

## Features

- STFT-based spectral morphing (FFT size 2048, hop 512)
- Sidechain-guided target spectrum
- Envelope modulation and envelope shaping controls
- Transient bypass blend for attack preservation
- Offline render synchronization path (no FIFO/thread drift)
- Mode-aware plugin delay compensation (PDC)

## Requirements

- CMake 3.20 or newer
- C++20 compiler toolchain
- Git
- Internet access for dependency fetch (first configure)

## Build

Dependencies are fetched automatically through CMake FetchContent:

- JUCE 8.0.4
- Eigen 3.4.0

Configure and build:

```bash
cmake -S . -B build
cmake --build build --config Release
```

Typical output locations on Windows:

- VST3: `build/E1Morph_artefacts/Release/VST3/E1Morph.vst3`
- Standalone: `build/E1Morph_artefacts/Release/Standalone/E1Morph.exe`

## I/O Layout

- Main Input: mono or stereo
- Sidechain Input: optional mono or stereo
- Output: matches main input layout

## Notes

- Real-time mode uses asynchronous worker processing with lock-free FIFOs.
- Offline export runs a synchronous processing path in `processBlock` for
  sample-accurate alignment.
- Latency reporting is updated based on render mode for accurate DAW PDC.

## License

This project is licensed under the MIT License.
See [LICENSE](LICENSE).

## Third-Party Licenses

JUCE and Eigen are third-party dependencies with their own licenses.
Please review their upstream license terms before distribution.
