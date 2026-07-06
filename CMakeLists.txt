cmake_minimum_required(VERSION 3.31)
project(dx11_hook)

set(CMAKE_CXX_STANDARD 20)

add_definitions(-DUNICODE -D_UNICODE)

# 关键：静态链接 MinGW 运行时（防止带 libwinpthread-1.dll 等）
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static")

# 头文件
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/ext/minhook
        ${CMAKE_CURRENT_SOURCE_DIR}/ext/minhook/hde)

# 源文件
file(GLOB MY_SRC src/*.cpp)
set(MH_SRC
        ext/minhook/buffer.c
        ext/minhook/hook.c
        ext/minhook/trampoline.c
        ext/minhook/hde/hde64.c
)

add_library(dx11_hook SHARED ${MY_SRC} ${MH_SRC})

target_link_libraries(dx11_hook dxgi d3d11 dxgi windowscodecs ole32)