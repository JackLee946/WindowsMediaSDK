#pragma once

// Install a process-wide crash handler that writes a minidump to exe directory.
// The dump file name will look like: crash_YYYYMMDD_HHMMSS.dmp
void InstallMiniDumpHandler();


