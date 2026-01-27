# example

this example shows how easy it is to use libhaze. simply call `hazeInitialize()` and it'll start the mtp server in the background.

remember to call `hazeExit()` before exiting!

if your code is `C` (like the example), then you will need to explicitly link std along with libhaze like so `LIBS := -lhaze -lstdc++ -lnx`.

if your code is `C++`, then there is no need to explicitly link stl `LIBS := -lhaze -lnx`.
