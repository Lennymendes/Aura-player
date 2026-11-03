// NX Player — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#import <Foundation/Foundation.h>
#include "SystemMonitor.h"

namespace nx
{

void PlaybackActivity::setPlaying (bool playing)
{
    if (playing == (token != nullptr)) return;
    if (playing)
    {
        id<NSObject> activity = [[NSProcessInfo processInfo] beginActivityWithOptions: NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
                                                                               reason: @"NX Player : lecture audio"];
        token = (__bridge_retained void*) activity;
    }
    else
    {
        id<NSObject> activity = (__bridge_transfer id<NSObject>) token;
        token = nullptr;
        [[NSProcessInfo processInfo] endActivity: activity];
    }
}

} // namespace nx
