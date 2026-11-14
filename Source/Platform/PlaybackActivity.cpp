// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "SystemMonitor.h"

#if ! JUCE_MAC
namespace nx
{
// Windows / Linux: no App Nap equivalent; the decoder thread runs at high priority on its own.
void PlaybackActivity::setPlaying (bool playing) { token = playing ? this : nullptr; }
}
#endif
