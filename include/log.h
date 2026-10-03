#pragma once

// Append-only log at sdmc:/3ds/mw3ds/log.txt. Flushed every line so the
// file survives crashes and the emulator being killed.
void logInit();
// Also send every line over UDP to "host port" read from hostFile (no file: no network log)
void logNetStart(const char* hostFile);
// The socket service, started once for the log and the dev updater
bool logNetEnsure();
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// "monitor: ..." for something an engine-side check found (tests count these lines): each key once per run,
// however often it happens (a missing texture used in a hundred cells is one line). Any thread.
void monitorOnce(const char* key, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void logExit();
// Called with each line (after it is written); nullptr to stop. Loading screens use it to show
// progress, so a load that hangs shows where it stopped.
void logSetHook(void (*hook)(const char* line));

// Watchdog: the main thread marks where it is (MARK("name")) and counts frames; a low-priority thread
// logs the last mark when frames stop for a few seconds (a hang on hardware says where it stopped)
extern const char* volatile g_mainAt;
extern volatile unsigned g_mainFrames;
extern const char* volatile g_workerAt;     // the streaming thread's mark
#define MARK(where) (g_mainAt = (where))
void watchdogStart();
// Crashes (data / prefetch aborts, running out of memory, std::terminate) write crash.txt with where
// the threads were; install on each thread (slot 0 main, 1 streaming)
void crashHandlerInstall(int slot);
