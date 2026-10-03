
#define UnixNow Int(ExecAndGetFirstLine( \
  GetEnv("SystemRoot") + "\System32\WindowsPowerShell\v1.0\powershell.exe", \
  "-NoProfile -NonInteractive -Command [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()", ""))

#define BuildSlot (UnixNow - 1790812800) / 600
#define MyAppVersion "1.0." + Str(BuildSlot)

[Setup]
AppName=Orly's Draw-A-Story
AppVersion={#MyAppVersion}
DefaultDirName="{autopf32}\Orly's Draw-A-Story"
DefaultGroupName="Orly's Draw-A-Story"
Compression=lzma/ultra
OutputBaseFilename="Orly's Draw-A-Story-{#MyAppVersion}"
OutputDir=tmp
SetupIconFile="images\newicon.ico"

[Files]
Source: "tmp\Orly's Draw-A-Story\*"; DestDir: "{app}"

[Icons]
Name: "{group}\Orly's Draw-A-Story"; Filename: "{app}\Orly32.exe"; IconFilename: "images\newicon.ico"
