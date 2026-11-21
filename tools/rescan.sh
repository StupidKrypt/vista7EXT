#!/usr/bin/env bash
# Re-run the import-gap scans over the downloaded programs (out/apps) against
# the copies of Vista, 7 and 8 (out/apps/sys): every program file
# (out/gaps/scan2.json) and the start chain of each main program listed in
# tests/vm/trace-apps.txt (out/gaps/start.json).
cd "$(dirname "$0")/.." || exit 1
A=out/apps
SYS=vista=vista64-deps,win7=win7x64-deps,win8=win8x64
mkdir -p out/gaps
python3 -I tools/gapscan.py --sys $SYS out/gaps/scan2.json \
  brave=$A/x/brave discord=$A/x/discord edge=$A/x/edge-x64 edge=$A/x/edge-x86 epic=$A/x/epic firefox=$A/x/firefox \
  steam=$A/x/steam spotify-setup=$A/dl/spotify/SpotifySetup.exe discord-setup=$A/dl/discord/DiscordSetup.exe \
  steam-setup=$A/dl/steam/SteamSetup.exe vscode=$A/x2/vscode telegram=$A/x2/telegram slack=$A/x2/slack/pkg \
  python-setup=$A/dl/python/python-3.12.10-amd64.exe nodejs=$A/x2/nodejs powershell=$A/x2/powershell godot=$A/x2/godot \
  obs=$A/x2/obs chrome=$A/x2/chrome/browser zoom=$A/x2/zoom/app signal=$A/x2/signal/app obsidian=$A/x2/obsidian/app \
  localsend=$A/x2/localsend java21=$A/x2/temurin21 \
  dotnet8-setup=$A/dl/dotnet8desktop/windowsdesktop-runtime-8.0.34-win-x64.exe gimp-setup=$A/dl/gimp/gimp-3.0.6-setup.exe \
  battlenet-setup=$A/dl/battlenet/Battle.net-Setup.exe eaapp-setup=$A/dl/eaapp/EAappInstaller.exe ubisoft=$A/x2/ubisoft \
  supertuxkart=$A/x2/supertuxkart luanti=$A/x2/luanti64 luanti=$A/x2/luanti32 xonotic=$A/x2/xonotic "$@" | tail -1
args=()
while IFS='|' read -r name arch dir exe rest; do
  case $name in ''|'#'*) continue;; esac
  args+=("$name=$dir::${exe//\\//}")
done < tests/vm/trace-apps.txt
python3 -I tools/startcheck.py --sys $SYS out/gaps/start.json "${args[@]}" | tail -1
