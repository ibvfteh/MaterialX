set(materialxremote_source
    
    MaterialCatalog.cpp
    External/jsoncpp/src/lib_json/json_reader.cpp
    External/jsoncpp/src/lib_json/json_value.cpp
    External/jsoncpp/src/lib_json/json_writer.cpp
    RemoteServer.cpp
    RemoteSession.cpp
    RemoteViewer.cpp)

set(materialxremote_headers
    
    MaterialCatalog.h
    RemoteServer.h
    Types.h
    RemoteSession.h
    RemoteViewer.h)

add_library(MaterialXRemote ${materialxremote_source} ${materialxremote_headers})

target_include_directories(MaterialXRemote
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/External/cpp-httplib>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/External/jsoncpp/include>)

target_link_libraries(MaterialXRemote
    PUBLIC
    MaterialXViewLib
    nanogui)

add_executable(MaterialXRemoteServer main.cpp)

target_link_libraries(MaterialXRemoteServer
    PRIVATE
    MaterialXRemote)
