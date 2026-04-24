# clang-cl bundled with Visual Studio — requires VSINSTALLDIR to be set (VS Developer Command Prompt).
set(CMAKE_C_COMPILER   "$ENV{VSINSTALLDIR}VC/Tools/Llvm/x64/bin/clang-cl.exe" CACHE STRING "")
set(CMAKE_CXX_COMPILER "$ENV{VSINSTALLDIR}VC/Tools/Llvm/x64/bin/clang-cl.exe" CACHE STRING "")
set(CMAKE_LINKER       "$ENV{VSINSTALLDIR}VC/Tools/Llvm/x64/bin/lld-link.exe" CACHE STRING "")
