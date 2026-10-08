#!/bin/sh
# Viper Racing on Linux: starts the game with the viperport engine. Lives in the game folder, beside race.exe.
#
#   ./viperport.sh              play
#   ./viperport.sh --check      check this race.exe can run (prints a report, starts nothing)
#   ./viperport.sh --menu       add Viper Racing to your desktop's application menu
#
# Anything else is passed on to the game. The log is viperport.log in this folder; settings are in viperport.ini.
here=$(dirname "$(readlink -f "$0")")
cd "$here" || exit 1

# race.exe in any case (a copy from the disc may be RACE.EXE)
exe=$(find . -maxdepth 1 -type f -iname race.exe | head -n 1)
if [ -z "$exe" ]; then
    echo "viperport.sh: no race.exe in $here -- put this package's files in your Viper Racing folder (see README-linux.txt)"
    exit 1
fi
exe=${exe#./}

if [ "${1:-}" = --menu ]; then
    apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
    mkdir -p "$apps"
    cat > "$apps/viper-racing.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Viper Racing
Comment=Viper Racing (1998) on the viperport engine
Exec="$here/viperport.sh"
Path=$here
Terminal=false
Categories=Game;
EOF
    echo "added $apps/viper-racing.desktop"
    exit 0
fi

if [ "${1:-}" = --check ]; then
    exec ./viperport --check "$exe"
fi

# one quick look before the window opens: a race.exe the engine can't run says why here, not in the log
if ! ./viperport --probe "$exe" > /dev/null; then
    cat viperport-probe.txt 2>/dev/null
    exit 1
fi
exec ./viperport --race "$exe" "$@"
