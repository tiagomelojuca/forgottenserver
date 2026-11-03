set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# GCC 15+ defaults to C23, where `bool` is a keyword. The 2021-era
# libmariadb pinned by the baseline typedefs `bool`, so build it as C17.
if(PORT STREQUAL "libmariadb")
    set(VCPKG_C_FLAGS "-std=gnu17")
    set(VCPKG_CXX_FLAGS "")
endif()
