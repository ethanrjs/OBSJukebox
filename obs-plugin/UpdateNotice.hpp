#pragma once
#include <obs-module.h>

namespace update_notice {
void start();
void stop();
bool addProperties(obs_properties_t* props);
}
