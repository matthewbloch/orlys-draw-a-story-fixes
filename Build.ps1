$PSNativeCommandUseErrorActionPreference = $true
$ErrorActionPreference = "Stop"

# We just use --force to ensure it still succeeds if the package is
# already installed.
winget install -s winget --accept-package-agreements --force 7Zip.7Zip
winget install -s winget --accept-package-agreements --force Python.Python.3.9
#winget install -s winget --accept-package-agreements --force MSYS2.MSYS2
winget install -s winget --accept-package-agreements --force JRSoftware.InnoSetup

$env:CHERE_INVOKING="1"
$env:MSYSTEM="MINGW32"
& C:\msys64\usr\bin\bash -lc "pacman -S --noconfirm mingw-w64-i686-gcc"
& C:\msys64\usr\bin\bash -lc "./Build.sh"
