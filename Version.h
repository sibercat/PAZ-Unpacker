#pragma once

#ifndef _VERSION_H_
#define _VERSION_H_

// Single source of truth for the application version.
//
// Included by both Main.h and PAZ-Unpacker.rc, so the resource metadata, the
// About box and the GitHub update check can never drift apart. The update
// check compares APP_VERSION_A against the latest release tag -- if that copy
// goes stale the app silently stops reporting updates, which is why this lives
// in one place. Keep it free of C++ so rc.exe can parse it.

#define APP_VERSION_MAJOR 2
#define APP_VERSION_MINOR 6
#define APP_VERSION_PATCH 0

#define APP_VERSION_A     "2.6.0"
#define APP_VERSION       L"" APP_VERSION_A

#endif
