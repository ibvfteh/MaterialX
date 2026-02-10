option(MATERIALX_REMOTE_EGL_ONLY "Build Remote with EGL-only backend (compile-time selection)" OFF)

set(materialxremote_source
    MaterialCatalog.cpp
    External/jsoncpp/src/lib_json/json_reader.cpp
    External/jsoncpp/src/lib_json/json_value.cpp
    External/jsoncpp/src/lib_json/json_writer.cpp
    RemoteViewerCommon.cpp
    RemoteServer.cpp
    RemoteSession.cpp)

if(MATERIALX_REMOTE_EGL_ONLY)
    list(APPEND materialxremote_source RemoteViewerEgl.cpp)
else()
    list(APPEND materialxremote_source RemoteViewer.cpp)
endif()

set(materialxremote_headers
    MaterialCatalog.h
    RemoteServer.h
    Types.h
    RemoteViewerCommon.h
    RemoteSession.h
    RemoteViewer.h)

if(MATERIALX_REMOTE_EGL_ONLY)
    list(APPEND materialxremote_headers RemoteViewerEgl.h)
endif()

add_library(MaterialXRemote ${materialxremote_source} ${materialxremote_headers})

if(MATERIALX_REMOTE_EGL_ONLY)
    target_compile_definitions(MaterialXRemote PUBLIC MATERIALX_REMOTE_EGL_ONLY=1)
endif()

target_include_directories(MaterialXRemote
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/External/cpp-httplib>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/External/jsoncpp/include>)

if(MATERIALX_REMOTE_EGL_ONLY)
    target_link_libraries(MaterialXRemote
        PUBLIC
        MaterialXRenderGlsl
        MaterialXRenderHw
        MaterialXGenGlsl
        MaterialXFormat
        MaterialXCore
        MaterialXRender)
else()
    target_link_libraries(MaterialXRemote
        PUBLIC
        MaterialXViewLib
        nanogui)
endif()

add_executable(MaterialXRemoteServer main.cpp)

target_link_libraries(MaterialXRemoteServer
    PRIVATE
    MaterialXRemote)

if(MATERIALX_REMOTE_EGL_ONLY)
    target_compile_definitions(MaterialXRemoteServer PUBLIC MATERIALX_REMOTE_EGL_ONLY=1)
endif()
