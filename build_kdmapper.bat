@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd kdmapper-master\kdmapper-master\kdmapper
echo [BUILDING KDMAPPER]
cl.exe /nologo /O2 /EHsc /std:c++17 /D_UNICODE /DUNICODE /Iinclude main.cpp intel_driver.cpp kdmapper.cpp portable_executable.cpp service.cpp utils.cpp KDSymbolsHandler.cpp version.lib User32.lib Kernel32.lib Advapi32.lib /link /SUBSYSTEM:CONSOLE /OUT:..\..\..\kdmapper.exe
cd ..\..\..
