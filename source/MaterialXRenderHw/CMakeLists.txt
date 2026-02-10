
file(GLOB materialx_source "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB materialx_headers "${CMAKE_CURRENT_SOURCE_DIR}/*.h*")

if(MATERIALX_REMOTE_EGL_ONLY)
    set(MATERIALX_EGL_ONLY ON)
endif()

if(MATERIALX_EGL_ONLY)
    list(FILTER materialx_source EXCLUDE REGEX "/SimpleWindowLinux\\.cpp$")
    list(FILTER materialx_source EXCLUDE REGEX "/WindowWrapper\\.cpp$")
endif()

if(APPLE)
    if (NOT MATERIALX_BUILD_APPLE_EMBEDDED)
        find_library(COCOA_FRAMEWORK Cocoa)
    endif()
elseif(UNIX AND NOT MATERIALX_EGL_ONLY)
    find_package(X11 REQUIRED)
    # Note - can't just require the Xt component because FindX11 in cmake 3.1
    # doesn't support it
    if(NOT X11_Xt_FOUND)
        message(FATAL_ERROR "Error in building MaterialXRenderHw: Xt was not found")
    endif()
endif()

mx_add_library(MaterialXRenderHw
    SOURCE_FILES
        ${materialx_source}
    HEADER_FILES
        ${materialx_headers}
    MTLX_MODULES
        MaterialXRender
    EXPORT_DEFINE
        MATERIALX_RENDERHW_EXPORTS
    ADD_OBJECTIVE_C_CODE)

if(MATERIALX_EGL_ONLY)
    target_compile_definitions(${TARGET_NAME} PUBLIC MATERIALX_EGL_ONLY)
endif()

if(APPLE)
    target_link_libraries(${TARGET_NAME}
            PUBLIC
            "-framework Foundation"
            "-framework Metal")
    if (NOT MATERIALX_BUILD_APPLE_EMBEDDED)
        target_link_libraries(${TARGET_NAME}
                PUBLIC
                "-framework Cocoa"
        )
    endif()
elseif(UNIX AND NOT MATERIALX_EGL_ONLY)
    target_link_libraries(${TARGET_NAME}
            PUBLIC
            X11::X11
            X11::Xt)
endif()
