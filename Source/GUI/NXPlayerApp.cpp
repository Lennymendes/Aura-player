// Aura — Copyright (c) 2026 NX Audio. Proprietary, all rights reserved (see LICENSE).
#include "NXPlayerApp.h"
#include "../Core/Diagnostics.h"
#include "AppController.h"
#include "MainComponent.h"
#include "../UI/SplashScreen.h"
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <cstdio>
#endif

namespace nx
{

static NXPlayerApplication* instance = nullptr;

NXPlayerApplication::NXPlayerApplication() = default;
NXPlayerApplication::~NXPlayerApplication() = default;

void NXPlayerApplication::initialise (const juce::String& commandLine)
{
    instance = this;
    auto args = juce::StringArray::fromTokens (commandLine, true);
    args.removeEmptyStrings();

    const int st = args.indexOf ("--selftest");
    if (st >= 0)
    {
       #if JUCE_WINDOWS
        // GUI-subsystem executable: print into the console that started us (CI, cmd, PowerShell).
        if (AttachConsole (ATTACH_PARENT_PROCESS) || AllocConsole())
        {
            FILE* f = nullptr;
            freopen_s (&f, "CONOUT$", "w", stdout);
            freopen_s (&f, "CONOUT$", "w", stderr);
            SetConsoleOutputCP (CP_UTF8);
        }
       #endif
        const auto filter = args[st + 1].unquoted();
        const int failures = nxtest::runAll (filter);
        setApplicationReturnValue (failures == 0 ? 0 : 1);
        quit();
        return;
    }

    diagnostics::installCrashHandler();
    controller = std::make_unique<AppController>();
    // a crash since the last launch: say where to get the logs (once per crash)
    if (const auto crash = diagnostics::lastCrashTime(); crash.toMilliseconds() > (juce::int64) controller->settings().get ("lastCrashSeen", 0))
    {
        controller->settings().set ("lastCrashSeen", crash.toMilliseconds());
        juce::Timer::callAfterDelay (2500, [] { if (auto* c = getController()) c->showMessage ("Aura s'est fermé de façon inattendue. Réglages › À propos › Exporter les journaux, puis envoyez le fichier au support."_t); });
    }
    if (auto* content = controller->getMainComponent())       // launch animation over the main component (1.5 s)
        splash = std::make_unique<SplashScreen> (*content);   // hides itself at the end, freed at shutdown
    juce::StringArray files;
    for (auto& a : args) if (! a.startsWith ("-")) files.add (a.unquoted());
    if (! files.isEmpty()) controller->openFiles (files);
}

void NXPlayerApplication::shutdown()
{
    splash.reset();
    controller.reset();
    instance = nullptr;
}

void NXPlayerApplication::anotherInstanceStarted (const juce::String& commandLine)
{
    if (controller == nullptr) return;
    auto args = juce::StringArray::fromTokens (commandLine, true);
    juce::StringArray files;
    for (auto& a : args) if (a.isNotEmpty() && ! a.startsWith ("-")) files.add (a.unquoted());
    if (! files.isEmpty()) controller->openFiles (files);
}

AppController* NXPlayerApplication::getController()
{
    return instance != nullptr ? instance->controller.get() : nullptr;
}

} // namespace nx
