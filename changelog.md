## v1.2.2
- Fixed a crash when opening a second Jukebox menu
- Fixed OBS checkboxes disappearing and false "OBS download failed" errors
- OBS now plays menu music and other music outside levels
- Smoother OBS audio: fewer clicks and crackles, better sync, and no cutting out during brief freezes

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
- Multiple GD instances no longer fight over OBS
- Fixed SFX with surround sound, and doubled audio when the source is in more than one scene
- Installer and uninstaller improvements: adding GD Sounds to scenes is optional, and admin rights are only requested when needed
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
