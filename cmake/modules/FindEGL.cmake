find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(EGL egl)
endif()

find_path(EGL_INCLUDE_DIR EGL/egl.h /usr/include /usr/include/EGL)
find_library(EGL_LIBRARY EGL)

set(EGL_LIBRARIES ${EGL_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EGL DEFAULT_MSG EGL_LIBRARY EGL_INCLUDE_DIR)

mark_as_advanced(EGL_INCLUDE_DIR EGL_LIBRARY)

if(EGL_FOUND AND NOT TARGET EGL::EGL)
    add_library(EGL::EGL UNKNOWN IMPORTED)
    set_target_properties(EGL::EGL PROPERTIES
        IMPORTED_LOCATION "${EGL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${EGL_INCLUDE_DIR}")
endif()
