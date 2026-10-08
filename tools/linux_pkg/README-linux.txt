Viper Racing on Linux -- the viperport engine
=============================================

viperport runs Viper Racing (MGI / Sierra, 1998) natively on Linux: no Wine. It is a rewrite of the
whole game, function by function, checked against the original, with an OpenGL renderer, SDL2 sound
and input, and the original's limits lifted. It needs your own copy of the game.

Version: @VERSION@


What you need
-------------
* Viper Racing version 1.0 -- the race.exe from the CD, unpatched. The 1.1 / 1.2.x patches
  (race.bin) are not supported on Linux.
* A 64-bit or 32-bit x86 Linux with glibc 2.35 or newer (Ubuntu 22.04+, Linux Mint 21+,
  Debian 12+, Fedora 36+, any current Arch).
* Your distro's 32-bit runtime and graphics drivers (the game is a 32-bit program). SDL2 is
  included in lib/; the rest comes from one install line:

  Debian / Ubuntu / Mint:
    sudo dpkg --add-architecture i386 && sudo apt update
    sudo apt install @APT_PACKAGES@

  Fedora (not yet tested):
    sudo dnf install @DNF_PACKAGES@

  Arch (enable [multilib] in /etc/pacman.conf first; not yet tested):
    sudo pacman -S @PACMAN_PACKAGES@


Installing
----------
1. Copy the game's files to a folder you can write to, e.g. ~/Games/ViperRacing. From the CD,
   copy the whole Data folder's contents (race.exe and everything beside it):
     mkdir -p ~/Games/ViperRacing
     cp -r /media/$USER/<your CD>/Data/. ~/Games/ViperRacing/
     chmod -R u+w ~/Games/ViperRacing
   (Files copied from a CD are read-only until the chmod.)
2. Put this package's files in that same folder, beside race.exe:
     tar xzf viperport-linux-*.tar.gz --strip-components=1 -C ~/Games/ViperRacing
3. Run it:
     ~/Games/ViperRacing/viperport.sh
   To add it to your application menu:  ~/Games/ViperRacing/viperport.sh --menu


Good to know
------------
* Settings: viperport.ini (made on the first run, beside race.exe). The game's own options and
  saves go to Config/.
* Log: viperport.log beside race.exe -- the first place to look if something goes wrong.
* Performance: put  perf=1  under  [debug]  in viperport.ini and the log shows where each frame's
  time goes, every 5 seconds.
* Multiplayer: TCP/IP (UDP) play works with Windows players -- tested on a LAN. IPX, serial and
  modem are Windows-only. Each player sees every car with their OWN copy of
  shared files -- your paint job and hornball look on everyone's car, as in the original game.
* Controllers work in races; the menus use the mouse and keyboard, as in the original.


Troubleshooting
---------------
* "no race.exe": the package's files aren't in the game folder -- see Installing, step 2.
* Nothing happens / a library is missing: run  ./viperport.sh --check  in a terminal, and check
  the install line above went through. "ld-linux.so.2: not found" or "No such file" for
  ./viperport means the 32-bit runtime isn't installed.
* No controller: the 32-bit libudev isn't installed (it's in the install line above); the log
  says "controllers can't start". The mouse and keyboard still work.
* The window opens black or the game is slow: check 32-bit Mesa is installed (glxinfo32 or the
  log's "GL" lines show the renderer); a software renderer (llvmpipe) is much slower.


Licences
--------
SDL2 (lib/libSDL2-2.0.so.0) is (c) Sam Lantinga, zlib licence: LICENSES/SDL2-LICENSE.txt.
The engine is statically linked with GCC's libstdc++ and libgcc under the GCC Runtime Library
Exception: LICENSES/GCC-RUNTIME-EXCEPTION.txt.
Viper Racing is (c) Sierra / MGI; this package contains none of the game's files.
