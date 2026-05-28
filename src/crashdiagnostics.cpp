// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "crashdiagnostics.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

// We deliberately avoid juce::FileLogger and std::ostream in this file: when
// a fatal signal fires we want the crash trace and breadcrumbs on disk
// before the process is killed, and JUCE's logger is buffered through layers
// (juce::FileOutputStream, optional juce::String concatenations using the
// heap) that may not survive a corrupted state. Everything here goes
// through low-level POSIX write() + fsync() to a single dedicated fd kept
// open for the lifetime of the process.

namespace element::diagnostics {

namespace {
// File descriptor for the crash log. Opened once at install time and never
// closed -- the OS reclaims it at process exit. A value of -1 means
// diagnostics are not installed (e.g. open() failed).
std::atomic<int> crashFd { -1 };

inline void writeRaw (int fd, const char* p, size_t n) noexcept
{
    // Partial writes are possible; loop until done. write() and fsync() are
    // both async-signal-safe per POSIX, so this is fine to call from a
    // signal handler.
    while (n > 0)
    {
        const auto w = ::write (fd, p, n);
        if (w <= 0)
            break;
        p += (size_t) w;
        n -= (size_t) w;
    }
}

inline void writeStr (int fd, const char* s) noexcept
{
    writeRaw (fd, s, std::strlen (s));
}

inline void writeInt (int fd, long v) noexcept
{
    char buf[32];
    int n = std::snprintf (buf, sizeof (buf), "%ld", v);
    if (n > 0) writeRaw (fd, buf, (size_t) n);
}

inline void writeHex (int fd, unsigned long long v) noexcept
{
    char buf[32];
    int n = std::snprintf (buf, sizeof (buf), "0x%llx", v);
    if (n > 0) writeRaw (fd, buf, (size_t) n);
}

const char* sigName (int sig) noexcept
{
    switch (sig)
    {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGTRAP: return "SIGTRAP";
        default:      return "?";
    }
}

void signalHandler (int sig, siginfo_t* info, void* /*ctx*/) noexcept
{
    const int fd = crashFd.load (std::memory_order_acquire);
    if (fd >= 0)
    {
        writeStr (fd, "\n\n!!! FATAL sig=");
        writeInt (fd, sig);
        writeStr (fd, " (");
        writeStr (fd, sigName (sig));
        writeStr (fd, ") addr=");
        writeHex (fd, info ? (unsigned long long) info->si_addr : 0ull);
        writeStr (fd, "\n");

        // Capture and dump the native stack backtrace. `backtrace_symbols_fd`
        // is documented as async-signal-safe on macOS (man page) and writes
        // resolved frame names directly without allocating into a returned
        // array, which is exactly what we need from a signal handler.
        void* frames[64];
        const int n = backtrace (frames, 64);
        backtrace_symbols_fd (frames, n, fd);

        // Force the writes to the disk before the kernel kills the process.
        fsync (fd);
    }

    // Re-raise the signal so the default action (terminate + crash report)
    // still runs, in case macOS's CrashReporter wants to record it too.
    std::signal (sig, SIG_DFL);
    std::raise (sig);
}
} // namespace

//==============================================================================
void installCrashDiagnostics (const juce::File& logFile)
{
    logFile.getParentDirectory().createDirectory();

    const int fd = ::open (logFile.getFullPathName().toRawUTF8(),
                            O_WRONLY | O_APPEND | O_CREAT,
                            0644);
    if (fd < 0)
        return;
    crashFd.store (fd, std::memory_order_release);

    // Session header to separate runs in the file.
    auto header = juce::String ("\n==== Element session started ")
                  + juce::Time::getCurrentTime().toString (true, true)
                  + " pid=" + juce::String ((long) ::getpid()) + " ====\n";
    writeRaw (fd, header.toRawUTF8(), (size_t) header.getNumBytesAsUTF8());
    fsync (fd);

    struct sigaction sa {};
    sa.sa_sigaction = signalHandler;
    sigemptyset (&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;

    for (int sig : { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTRAP })
        sigaction (sig, &sa, nullptr);
}

void breadcrumb (juce::StringRef tag, juce::StringRef msg)
{
    const int fd = crashFd.load (std::memory_order_acquire);
    if (fd < 0)
        return;

    // [hhmmss.mmm tag] msg\n
    const auto t = juce::Time::getCurrentTime();
    char prefix[64];
    int n = std::snprintf (prefix, sizeof (prefix), "[%02d:%02d:%02d.%03d ",
                            t.getHours(), t.getMinutes(), t.getSeconds(),
                            t.getMilliseconds());
    if (n > 0) writeRaw (fd, prefix, (size_t) n);
    writeRaw (fd, tag.text.getAddress(), std::strlen (tag.text.getAddress()));
    writeStr (fd, "] ");
    writeRaw (fd, msg.text.getAddress(), std::strlen (msg.text.getAddress()));
    writeStr (fd, "\n");
    fsync (fd);
}

} // namespace element::diagnostics
