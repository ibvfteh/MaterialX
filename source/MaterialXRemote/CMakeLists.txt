set(materialxremote_source
    RemoteSession.cpp
    RemoteViewer.cpp)

set(materialxremote_headers
    RemoteSession.h
    RemoteViewer.h)

add_library(MaterialXRemote ${materialxremote_source} ${materialxremote_headers})

target_include_directories(MaterialXRemote
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)

target_link_libraries(MaterialXRemote
    PUBLIC
    MaterialXViewLib
    nanogui)
