## Unreleased
- OBS follows speedhacks from 0.0001x to 16x; it used to stop at 0.25x and 4x
- OBS applies other mods' pitch shifters to music and sound effects in the same order as the game, building on @ethanrjs's pitch shifter support
- New setting: Pitch shift a different OBS song (on by default)
- Update the mod and the OBS plugin together; the new versions only work with each other

## v1.2.3
-  Level editor support
- Less notification spam
- Less background work during gameplay
- Fixed the macOS installer refusing to install properly
- The installer can now undo installs made by earlier versions
- Fixed a Jukebox warning in the Geode log on every launch
- OBS refuses more kinds of network paths for safety
- The OBS plugin now asks you to update the mod if it’s paired with a v1.0 mod

Pretty much all of these were done thanks to @navinor, I appreciate the contributions so much! Mod is far more stable and polished than it was just a few days ago.

## v1.2.2
- Fixed a crash when opening a second Jukebox menu
- Fixed OBS checkboxes disappearing and false "OBS download failed" errors
- OBS now plays menu music and other music outside levels, and switches back to menu music more smoothly
- Smoother OBS audio: fewer clicks and crackles, better sync, and no cutting out during brief freezes
- OBS no longer shows the last level's name and attempt count after you leave it
- OBS only plays songs from local files

## v1.2.1
- OBS audio recovers on its own if it stops while the game is still playing
- Fixed a rare audio edge case

## v1.2.0
- Play no longer gets stuck when your OBS song isn't downloaded or you're offline; OBS plays the game song instead
- Smoother OBS audio with song triggers and song changes, with less background work
- Fixed a crash after updating Jukebox
- Jukebox list fixes: no OBS checkbox on "Download nongs" rows, and no flicker when changing the game song
- The offset setting now shows which level it applies to
- Song and level names with special characters show correctly in OBS
- Very long file paths now show a message
- Multiple GD instances no longer fight over OBS
- Fixed SFX with surround sound, and doubled audio when the source is in more than one scene
- Installer and uninstaller improvements: adding GD Sounds to scenes is optional, admin rights are only requested when needed, and installing works when OBS is minimized to the tray
- Mac: the plugin is less likely to be blocked by macOS security
- Linux: the installer checks for the right Jukebox version

## v1.1.0
- Better support for Song and Edit Song triggers, including multiple music channels, volume changes and fades
- Fixed audio sync issues and long OBS songs stopping after pausing
- Better SFX capture when in-game SFX are muted
- Improved installers, compatibility checks and save data
- Experimental Linux support through Proton
- Experimental macOS support for Apple Silicon

## v1.0.0
- Initial release.
