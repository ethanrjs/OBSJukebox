# OBS Jukebox

Play one song in Geometry Dash and a different one on your OBS stream. In Jukebox you pick a Game song and an OBS song for each level. OBS Jukebox sends the OBS song and the game's sound effects to OBS, so your viewers hear the OBS song while you hear the Game song.

## You also need the OBS plugin

This mod works only together with the OBS Jukebox plugin for OBS Studio, which adds a **GD Sounds** audio source to OBS.

1. Download the setup for your system from the [releases page](https://github.com/navinor/OBSJukebox/releases) and run it with Geometry Dash and OBS closed. It installs the plugin, and this mod too if needed. The setup needs Jukebox 3.8.0.
2. In OBS, add **GD Sounds** to your scene once (Sources, then +, then GD Sounds). On Windows, the setup can do this for you if you tick "Add GD Sounds to my existing OBS scenes".
3. Turn off any other capture of Geometry Dash's audio in OBS, or viewers hear the game twice. Keep Audio Monitoring off for GD Sounds.

## Using it

- In a level's Jukebox list, tick **Game** for the song you hear and **OBS** for the song your stream hears. Right-clicking a row also picks it for OBS.
- In the pause menu, the **Game / OBS** button switches the volume sliders between the game and OBS.
- GD Sounds shows what it is playing, or what it is waiting for, in its properties in OBS.

## Speedhack and pitch shifting

OBS follows speedhacks up to 16x. When another mod pitch shifts the game's audio with FMOD's pitch shifter, OBS applies the same shifters in the same order. A different OBS song follows them too; turn off **Pitch shift the OBS song** to keep it at its normal pitch.

## Updates

On Windows and macOS, the mod asks github.com once per launch whether a new OBS Jukebox is out, unless **Updates** is set to **Don't check**. The setting picks what happens next:

- **Ask me**: a small panel on the main menu offers the update.
- **Install when GD and OBS close**: the update installs the next time both are closed.
- **Install right away**: GD closes, the update installs, and GD reopens. If OBS is open or you are in a level, it waits until you close GD and OBS.

The mod and the OBS plugin update together, and the updater never closes OBS. On Linux, a panel links to the releases page instead.

## Requirements

- Windows, or a Mac with Apple Silicon (macOS 13 or later). Linux works only through Proton, with the experimental installer on the releases page.
- Jukebox 3.8.0. With any other Jukebox version, OBS plays the same song as the game until OBS Jukebox supports that version.
- Node IDs, from Geode's mod list. The setup does not install it.
- 64-bit OBS Studio. The plugin is built against OBS 32. Run OBS as your normal user, not as administrator.

## Credits

[Jukebox](https://github.com/Fleeym/jukebox) by Fleeym (MIT license).

Source code and bug reports: https://github.com/navinor/OBSJukebox
