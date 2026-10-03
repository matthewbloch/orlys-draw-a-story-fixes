param (
  [Parameter(Mandatory)]
  $ISO
)

$PSNativeCommandUseErrorActionPreference = $true
$ErrorActionPreference = "Stop"

$TMP="tmp"
$INSTALL="tmp\Orly's Draw-A-Story"

# Make the output directory
if (!(Test-Path -PathType Container $INSTALL)) {
    New-Item -Type Directory $INSTALL | Out-Null
}

# Copy this first, in case it's not built yet
Copy-Item winspool.dll "${INSTALL}"
Copy-Item orlyfix.ini "${INSTALL}"

# Use 7-Zip to extract the CD contents
& "C:\Program Files\7-Zip\7z" e -y -bso0 -bsp0 -o"${TMP}" "${ISO}" ARCHIVE\ARCHIVE.Z
# Use a Python program to extract the ancient InstallShield archive
python idecomp.py -C "${INSTALL}" "${TMP}\ARCHIVE.Z"
Remove-Item ${TMP}\ARCHIVE.Z
# Ensure we overwrite stub versions of SPR_*.MHK with full ones from CD
& "C:\Program Files\7-Zip\7z" e -y -bso0 -bsp0 -o"${INSTALL}" "${ISO}" ORLY\*.MHK
# Now build the installer and show the result
& "${ENV:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe" Orly.iss
Start-Process ${TMP}
