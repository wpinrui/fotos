cmake_minimum_required(VERSION 3.20)
project(fotos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/App.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/ImageLoader.cpp
    src/ImageCache.cpp
    src/FolderNavigator.cpp
)

set(HEADERS
    src/pch.h
    src/App.h
    src/Window.h
    src/Renderer.h
    src/ImageLoader.h
    src/ImageCache.h
    src/FolderNavigator.h
)

# Create executable
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS} resources/fotos.manifest)

# Precompiled header
target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)

# Link Windows libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    d2d1
    dwrite
    windowscodecs
    dwmapi
    shlwapi
    shell32
    ole32
    uuid
)

# Enable warnings
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive-)
endif()
