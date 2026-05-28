// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/core.hpp>

namespace element::diagnostics {

/** Install POSIX signal handlers that, on any fatal signal, write a stack
    trace to `logFile` via low-level write()+fsync() so the data survives
    even if the JUCE message loop or std::ostream buffers don't get a chance
    to flush. Also records a session header on install. */
void installCrashDiagnostics (const juce::File& logFile);

/** Append a single breadcrumb line to the crash log with explicit fsync.
    Thread-safe (POSIX write() is atomic for short messages on macOS/Linux).
    Use sparingly at "interesting" points so the file is not flooded. */
void breadcrumb (juce::StringRef tag, juce::StringRef msg);

#if JUCE_MAC
/** Install Cocoa/Objective-C uncaught-exception + std::terminate handlers.
    Catches plugin-editor crashes that bypass POSIX signals (typical for
    VST3/AU plugins built on AppKit/WebKit). Defined in
    crashdiagnostics_mac.mm. Call once at app startup AFTER
    installCrashDiagnostics. */
void installMacCrashHandlers();
#endif

} // namespace element::diagnostics
