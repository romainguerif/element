// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

// macOS-only complement to crashdiagnostics.cpp.
//
// On macOS, plugin editors built with Cocoa can throw Objective-C exceptions
// that bypass POSIX signal handlers and macOS's CrashReporter entirely:
// the process just disappears with no .ips file and no SIGSEGV. We register
// `NSSetUncaughtExceptionHandler` here so those exceptions still leave a
// breadcrumb + backtrace on disk before the runtime kills us.
//
// We also install a C++ std::terminate handler -- this catches uncaught
// `throw` from any C++ code path the audio engine touches.

#import <Foundation/Foundation.h>
#include <exception>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

#include "crashdiagnostics.hpp"

namespace element::diagnostics {

namespace {

void writeBacktrace() noexcept
{
    void* frames[64];
    const int n = backtrace (frames, 64);
    // Resolve to a temporary file descriptor that we know is open: write to
    // stderr (fd 2) as well, since we can't reach the crash fd from this
    // translation unit. The breadcrumb() call will also dump to crash.log.
    backtrace_symbols_fd (frames, n, STDERR_FILENO);
}

void objcUncaughtExceptionHandler (NSException* exception)
{
    @try
    {
        NSString* reason = [exception reason] ?: @"(no reason)";
        NSString* name   = [exception name]   ?: @"(no name)";
        const char* nameC   = [name   UTF8String] ?: "?";
        const char* reasonC = [reason UTF8String] ?: "?";

        char msg[1024];
        std::snprintf (msg, sizeof (msg),
                        "ObjC uncaught: %s -- %s", nameC, reasonC);
        breadcrumb ("crash", msg);

        // Symbolicated callstack from the exception itself, written one
        // frame per breadcrumb so each is fsync'd on its way to disk.
        NSArray* stack = [exception callStackSymbols];
        for (NSUInteger i = 0; i < [stack count] && i < 64; ++i)
        {
            const char* frame = [[stack objectAtIndex: i] UTF8String];
            if (frame) breadcrumb ("crash", frame);
        }

        // Also drop a raw backtrace to stderr as a last-resort signal in
        // case the breadcrumb call is somehow compromised.
        writeBacktrace();
    }
    @catch (...)
    {
    }

    // Fall through to the system default: re-raise so the OS still gets a
    // chance to write its own crash report.
    std::abort();
}

[[noreturn]] void cppTerminateHandler() noexcept
{
    breadcrumb ("crash", "C++ std::terminate fired");
    if (auto ex = std::current_exception())
    {
        try { std::rethrow_exception (ex); }
        catch (const std::exception& e)
        {
            char msg[512];
            std::snprintf (msg, sizeof (msg), "std::exception: %s", e.what());
            breadcrumb ("crash", msg);
        }
        catch (...)
        {
            breadcrumb ("crash", "non-std exception type");
        }
    }
    writeBacktrace();
    std::abort();
}

} // namespace

void installMacCrashHandlers()
{
    NSSetUncaughtExceptionHandler (&objcUncaughtExceptionHandler);
    std::set_terminate (&cppTerminateHandler);
    breadcrumb ("startup", "Mac handlers installed (NSUncaught + std::terminate)");
}

} // namespace element::diagnostics
