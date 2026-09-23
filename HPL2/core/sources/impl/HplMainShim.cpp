/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

// The process entry point, and nothing else.
//
// This lived in LowLevelSystemSDL.cpp, which is a problem on ELF and only
// there. A static archive hands the linker whole objects: LowLevelSystemSDL.o
// also carries the log writer and the SDL system implementation, so any program
// linking libHPL2 pulls that object in, entry point included. On Windows the
// entry point is WinMain, a symbol a console test never defines, so nothing
// collided and nobody noticed. On Linux it is `main`, and a test that defines
// its own main got:
//
//     multiple definition of `main';
//     LowLevelSystemSDL.cpp:83: first defined here
//
// Alone in its own translation unit, this object is pulled only when the link
// still needs `main` -- which is exactly when the program has not supplied one.
// A test with its own main silently gets its own; the game gets this one.
//
// IGNORE_HPL_MAIN is kept for the callers that still set it, but it is no
// longer load-bearing: it only ever applied to translation units compiled with
// it defined, and libHPL2 is built once, without it.

#include "system/Platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <clocale>
#include <langinfo.h>
#include <strings.h>
#include <unistd.h>
#endif

#ifndef IGNORE_HPL_MAIN
extern int hplMain(const hpl::tString &asCommandLine);

#ifdef _WIN32
#include <windows.h>
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
                   _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
  return hplMain(lpCmdLine);
}
#else
int main(int argc, char *argv[]) {
#ifdef __linux__
  if (!std::setlocale(LC_CTYPE, "")) {
    fprintf(stderr,
            "Can't set the specified locale! Check LANG, LC_CTYPE, LC_ALL.\n");
    return 1;
  }
  char *charset = nl_langinfo(CODESET);
  bool utf8_mode = (strcasecmp(charset, "UTF-8") == 0);
  if (!utf8_mode) {
    fprintf(stderr,
            "UTF-8 Charset %s available.\nCurrent LANG is %s\nCharset: %s\n",
            utf8_mode ? "is" : "not", getenv("LANG"), charset);
  }
#endif

  bool cwd = false;
  hpl::tString cmdline = "";
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-cwd") == 0) {
      cwd = true;
    } else if (strncmp(argv[i], "-psn", 4) == 0) {
      // skip "finder" process number
    } else {
      if (cmdline.length() > 0) {
        cmdline.append(" ").append(argv[i]);
      } else {
        cmdline.append(argv[i]);
      }
    }
  }

  if (!cwd) {
    hpl::tString dataDir = hpl::cPlatform::GetDataDir();

    chdir(dataDir.c_str());
  }

  return hplMain(cmdline);
}
#endif
#endif
