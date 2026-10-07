# Audio regression tests

Run `./scripts/tests/audio/run.ps1` from PowerShell on Windows with MSVC's C++ build tools and a Windows SDK installed. The tests require local UDP access. Build without running with `-BuildOnly` if the sandbox blocks loopback, then run `artifacts/audio-tests/playback-tests.exe` with loopback permitted from the repository root.

The harness compiles the real OBS receiver, playback worker, decoders and game bridge. Only OBS's registration and audio submission API are replaced with a recorder. WAV fixtures and binaries are generated under the ignored `artifacts/audio-tests` directory. No running OBS or Geometry Dash installation is needed.

Checks cover delayed position updates, sample timing for pause/resume/death/restart/song selection, epoch changes, gains, effects gating, v2/v3 compatibility, reordered packets and preservation of the sender's capture timestamp. This does not replace production SDK builds or a live game/OBS check.
