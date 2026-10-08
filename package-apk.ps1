# Package the headset app into an APK, and optionally install it and push the disc image.
#
#   .\build-android.ps1              # builds libprime.so
#   .\package-apk.ps1 -Install       # packages, installs, pushes the image
#
# The work is gcn-recomp's (tools/package-apk.ps1); this only says which game.
param([switch]$Install)
Set-Location $PSScriptRoot
& "$PSScriptRoot\gcn-recomp\tools\package-apk.ps1" -Name prime -Package com.example.prime `
    -Title 'Metroid Prime' -Install:$Install
