@echo off
if exist build2 rmdir /s /q build2
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -B build2 -DWITH_GPU=ON -DALLOW_UNSUPPORTED_MSVC_FOR_CUDA=ON
