cmake_minimum_required(VERSION 3.22)

project(imgui_root_android)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(${PROJECT_NAME}
    main.cpp
    ../../imgui.cpp
    ../../imgui_demo.cpp
    ../../imgui_draw.cpp
    ../../imgui_tables.cpp
    ../../imgui_widgets.cpp
    ../../backends/imgui_impl_opengl3.cpp
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    IMGUI_IMPL_OPENGL_ES3
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ../..
    ../../backends
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    EGL
    GLESv3
    nativewindow
    dl
    log
)
