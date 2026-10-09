# macOS trigger smoke

Run `build-mac.zsh`, then `python3 stage-mac.py`, then `python3 full-run-mac.py`.
The staging step copies the installed game into the repository's
`artifacts/trigger-smoke` directory and verifies sandbox write confinement before
launch. An existing running Geometry Dash process prevents a smoke launch.

Paths default to this checkout and the current user's Steam game installation.
Optional environment overrides:

- `OBS_JUKEBOX_ROOT`: repository checkout.
- `OBS_JUKEBOX_SMOKE_DIR`: isolated staging/output directory.
- `OBS_JUKEBOX_GAME_APP`: source Geometry Dash.app bundle.
- `GEODE_SDK`: SDK checkout (default `$HOME/geode`).
- `GEODE_BINDINGS_REPO_PATH` and `GEODE_CLI`: optional CMake dependency overrides.
- `CMAKE`: CMake executable (default `cmake` on PATH).
- `OBS_JUKEBOX_ARCH`: target architecture (default `arm64`).

The smoke staging directory must neither contain nor reside inside the installed
game bundle. Staged application and Geode links may not redirect writes outside
the isolated copy. The required built bridge, payload, plugin, and OBS runtime
validation harness retain their repository-relative output paths.
