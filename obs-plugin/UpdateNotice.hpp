#pragma once
#include <obs-module.h>

// Tells the player when the latest release needs another GD version. After a GD update the mod
// stops loading, so its updater can't, but OBS still loads this plugin.
namespace update_notice {
void start();
void stop();
// Adds the warning and the download button; false when no release needs another GD.
bool addProperties(obs_properties_t* props);
}
