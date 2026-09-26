cmake_minimum_required(VERSION 3.20)
project(ddnet-module-installer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ddnet-module-installer WIN32
    src/core.cpp
    src/installer.cpp
    src/gui.cpp
)

target_compile_definitions(ddnet-module-installer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# /utf-8: 源码是 UTF-8 无 BOM，MSVC 需要这个开关才能正确编译中文字面量
target_compile_options(ddnet-module-installer PRIVATE /utf-8 /W3 /EHsc)

target_link_libraries(ddnet-module-installer PRIVATE wininet comctl32 shell32 ole32 user32 gdi32)

# 把模块清单与模块目录拷到产物旁边，双击即用
# 只出 exe 单程序：不复制 README / modules / mods（首次运行时自检会创建 mods\src\client）
