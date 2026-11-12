CalcFront - an OpenFront-style territory conquest game for the TI-84 Plus CE
https://github.com/ArthusPVP/CalcFront

WHAT IS IN THIS ZIP
  CALCFRNT.8xp   the game
  maps/*.8xv     the 15 maps (send all of them, or only the ones you want)

1. INSTALL THE CE C LIBRARIES FIRST (required, free)
   CalcFront needs the standard "CE C libraries" (graphx, keypadc, fileioc, ...).
   Download clibs.8xg from:
       https://github.com/CE-Programming/libraries/releases/latest
   and send it to your calculator. If you skip this, the game shows
   "Need LibLoad".

2. SEND THE GAME AND THE MAPS
   Use TI Connect CE (Windows/macOS) or TiLP (Linux) to send CALCFRNT.8xp and
   the CF*.8xv files from the maps folder. Sending them to the Archive is fine
   and recommended: it keeps your RAM free.

3. RUN IT
   - OS 5.3 and 5.4: press [prgm], choose CALCFRNT, press [enter] twice.
   - OS 5.0 to 5.2: run Asm(prgmCALCFRNT) from the home screen.
   - OS 5.5 and newer: TI disabled assembly/C programs. The community tool
     arTIfiCE (https://yvantt.github.io/arTIfiCE/) brings them back; check its
     page for the OS versions it supports.

   The game needs about 120 KB of free RAM while it runs (98 KB for the program,
   20 KB of work memory). Archive other programs if you get "Not enough free RAM"
   or a memory error.

CONTROLS
  arrows          move the cursor        8 4 6 2   scroll the map
  + / -           zoom in / out          clear     back / pause
  y= window zoom trace graph   the 5 action keys shown on the bottom bar
  enter           attack the land under the cursor

LICENCES
  CalcFront code: GNU AGPL-3.0 (see LICENSE.txt). Source code:
  https://github.com/ArthusPVP/CalcFront
  Maps: derived from OpenFront's maps, CC BY-SA 4.0 (see maps/LICENSE.md).
  Built with the CE C toolchain (LGPL-3.0); the program contains the ZX0
  decompressor by Einar Saukas (BSD-3-Clause).
  CalcFront is an unofficial fan project, not affiliated with OpenFront.
