Viper Racing on Windows -- the viperport engine
===============================================

viperport is a rewrite of Viper Racing (MGI / Sierra, 1998), function by function, checked against
the original, with an OpenGL renderer at your screen's own resolution (widescreen), SDL2 sound
and input, and the original's limits lifted. It needs your own copy of the game.

This is the engine on its own. The Viper Racing Mod Manager (ViperModManager.exe, same release
page) installs the same files for you and adds a Play button -- use either, not both by hand.

Version: @VERSION@


What's in this zip
------------------
  viperport.exe    the standalone: runs the game on the engine alone (version 1.0 only)
  dinput.dll       the engine itself; also loaded by race.exe / race.bin when started as usual
  SDL2.dll         the SDL2 library the engine uses for the window, input and sound
  viperport.ini    the engine's settings
  LICENSES\        SDL2's licence


Installing
----------
1. Find your Viper Racing folder -- the one with race.exe (or race.bin) in it. If the game was
   installed from the CD, that's usually the Data folder inside the install, e.g.
   C:\Sierra\Viper Racing\Data.
2. Copy everything from this zip into that folder, beside race.exe. If Windows asks, replace the
   files (an older viperport install); keep your own viperport.ini if you changed it.
3. Start the game:
   * Version 1.0 (race.exe): run viperport.exe. Everything runs on the engine.
   * Version 1.1 or the community 1.2.x builds (race.bin): start the game the way you always do.
     The engine loads as dinput.dll and gives you the new renderer, sound and lifted limits.
   * Starting race.exe as usual on 1.0 works too (the same dinput.dll route), but viperport.exe
     is the full engine.

To uninstall, delete viperport.exe, dinput.dll, SDL2.dll, viperport.ini and viperport.log.


Good to know
------------
* Windows may say "Windows protected your PC" the first time: the files aren't signed with a
  paid certificate. Click "More info", then "Run anyway". Some antivirus tools also flag game
  mods that load as a DLL beside the game. The release page lists each file's SHA-256 checksum.
* Settings: viperport.ini. The game's own options and saves go to Config\.
* Log: viperport.log beside race.exe -- the first place to look if something goes wrong.
* Graphics: [graphics] in viperport.ini -- anisotropic=16 sharpens distant textures (switch on
  "filtering" and "mipmap" in the game's Graphics options too) and msaa=2 or 4 smooths edges
  (next start); fxaa=1 is a cheaper smoothing that softens the picture a little. All are 0,
  the original look, until you change them. The mod manager's
  Graphics choice sets all of this for you.
* Performance: put  perf=1  under  [debug]  in viperport.ini and the log shows where each
  frame's time goes, every 5 seconds.
* Multiplayer: Windows and Linux players can race each other over TCP/IP. Each player sees
  every car with their OWN copy of shared files -- your paint job and hornball look on everyone's
  car, as in the original game.
* Controllers: a game controller (Xbox, PlayStation and the like) works without setting it up,
  and can be plugged in or out at any time. In a race it drives alongside the keyboard: left
  stick steer, RT throttle, LT brake, A handbrake, B reverse, X horn, Y next camera, LB / RB
  shift down / up, right stick look left / right (click: look back), Start the pause menu (in
  it: D-pad, A choose, B back). In the menus the left stick moves the mouse pointer, A clicks,
  X right-clicks and B is Esc. If you map any control to the joystick in Options > Controls,
  races use your mapping instead, exactly as the original did. A wheel or plain joystick works
  through Options > Controls, as in the original.


Licences
--------
SDL2 (SDL2.dll) is (c) Sam Lantinga, zlib licence: LICENSES\SDL2-LICENSE.txt.
Viper Racing is (c) Sierra / MGI; this package contains none of the game's files.
