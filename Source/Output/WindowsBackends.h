// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#pragma once

#include "OutputBackend.h"

namespace nx
{
#if JUCE_WINDOWS
/** ASIO (PCM + DSD natif, when built with NX_ASIO_SDK_DIR) then WASAPI exclusive (integer formats). */
std::vector<std::unique_ptr<OutputBackend>> createWindowsBackends();
#endif
}
