@ECHO ON
set GYP_CHROMIUM_NO_ACTION=0
set GYP_DEFINES=target_arch=x64 clang=0 building_nw=1
set GYP_MSVS_VERSION=2017

START /B /WAIT git.exe pull

START /B /WAIT ninja.exe -C out/nw nwjs
START /B /WAIT ninja.exe -C out/Release_x64 node
START /B /WAIT ninja.exe -C out/nw copy_node
START /B /WAIT ninja.exe -C out/nw dump
START /B /WAIT ninja.exe -C out/nw dist
