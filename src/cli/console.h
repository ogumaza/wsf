// SPDX-License-Identifier: MIT

// The console that the command-line tools print to. Dropping files on wsfrip.exe or wsfplay.exe, or double-clicking
// one, opens a new console window for the tool. That window closes as soon as the tool exits.

#pragma once

namespace wsf
{

// Sets the Windows console to UTF-8 and restores its code page on exit. Does nothing on other platforms.
void SetUpConsole();

// If Windows opened a console window for the tool alone, waits for Enter before the tool exits. This keeps the output
// on screen. Does nothing elsewhere.
void KeepConsoleOpen();

} // namespace wsf
