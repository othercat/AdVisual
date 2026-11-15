cmake_minimum_required(VERSION 3.26)
cmake_policy(VERSION 3.26)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

if(CMAKE_HOST_APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Oldest supported macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "AdVisual macOS release architecture")
endif()
project(advisual LANGUAGES C CXX VERSION 1.1.0 HOMEPAGE_URL "https://github.com/othercat/AdVisual")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
# pkg-config's CPU/compiler flags do not apply to the Windows resource compiler.
get_target_property(gtkmm_compile_options PkgConfig::GTKMM INTERFACE_COMPILE_OPTIONS)
if(gtkmm_compile_options)
    set_property(TARGET PkgConfig::GTKMM PROPERTY INTERFACE_COMPILE_OPTIONS
        "$<$<COMPILE_LANGUAGE:C,CXX>:${gtkmm_compile_options}>")
endif()

if (WIN32)
    enable_language(RC)
	add_executable(advisual WIN32 src/main.cpp windows_resource.rc)
else()
	add_executable(advisual src/main.cpp)
endif()
file(GLOB_RECURSE src_files CONFIGURE_DEPENDS "src/*.cpp" "src/*.c")
target_sources(advisual PRIVATE ${src_files})
target_include_directories(advisual PRIVATE "include")
target_include_directories(advisual PRIVATE "src")
set_property(TARGET advisual PROPERTY CXX_STANDARD 20)
if(EXISTS "${CMAKE_SOURCE_DIR}/lib")
    target_link_directories(advisual PRIVATE ${CMAKE_SOURCE_DIR}/lib)
endif()

if (NOT TARGET libbinio)
	find_package(libbinio QUIET CONFIG)
    if(NOT TARGET libbinio::libbinio)
        pkg_check_modules(BINIO REQUIRED IMPORTED_TARGET libbinio)
        add_library(libbinio::libbinio ALIAS PkgConfig::BINIO)
    endif()
endif()

if (NOT TARGET adplug)
	find_package(adplug QUIET CONFIG)
    if(NOT TARGET adplug::adplug)
        pkg_check_modules(ADPLUG REQUIRED IMPORTED_TARGET adplug)
        add_library(adplug::adplug ALIAS PkgConfig::ADPLUG)
    endif()
endif()

target_link_libraries(advisual PRIVATE adplug::adplug)
target_link_libraries(advisual PRIVATE libbinio::libbinio)

target_link_libraries(advisual PRIVATE PkgConfig::GTKMM)

if(APPLE OR WIN32)
    set(portable_default ON)
else()
    set(portable_default OFF)
endif()
option(ADVISUAL_PORTABLE "Enable relocatable release packaging" ${portable_default})
if(ADVISUAL_PORTABLE)
    set(advisual_install_fallback "./")
else()
    set(advisual_install_fallback "${CMAKE_INSTALL_PREFIX}/")
endif()
function(advisual_build_settings target)
    target_compile_definitions(${target} PRIVATE INSTALL_PATH="${advisual_install_fallback}")
    if(WIN32)
        target_compile_definitions(${target} PRIVATE WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN)
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=build>")
    endif()
endfunction()
advisual_build_settings(advisual)
# Development binaries use the same relative resource layout as portable releases.
file(COPY icons themes advisual.ini docs.txt DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/share/advisual")

# Install first party executable, icons and themes.
install(TARGETS advisual DESTINATION bin
	PERMISSIONS WORLD_READ OWNER_WRITE OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_WRITE GROUP_EXECUTE WORLD_EXECUTE)

install(DIRECTORY icons DESTINATION share/advisual
	FILE_PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ GROUP_WRITE WORLD_READ
	DIRECTORY_PERMISSIONS OWNER_WRITE OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

install(DIRECTORY themes DESTINATION share/advisual
	FILE_PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ GROUP_WRITE WORLD_READ WORLD_EXECUTE
	DIRECTORY_PERMISSIONS OWNER_WRITE OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

install(FILES "docs.txt" DESTINATION share/advisual)
install(FILES "advisual.ini" DESTINATION share/advisual) # Default/Fallback ini file.

if (UNIX)
	install(FILES "advisual.desktop" DESTINATION share/applications)
	install(FILES "icons/advisual.svg" DESTINATION share/icons/hicolor/scalable/apps)
endif()

if(ADVISUAL_PORTABLE)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    set(ADVISUAL_RUNTIME_PREFIX "${GTKMM_PREFIX}" CACHE PATH "GTK runtime installation prefix")
    if(APPLE)
        find_program(HOMEBREW_EXECUTABLE brew REQUIRED)
        execute_process(COMMAND "${HOMEBREW_EXECUTABLE}" --prefix OUTPUT_VARIABLE homebrew_prefix OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        set(runtime_prefix "${homebrew_prefix}")
        set(portable_platform macos)
        set(portable_directory "AdVisual.app")
    elseif(WIN32)
        set(runtime_prefix "${ADVISUAL_RUNTIME_PREFIX}")
        set(portable_platform windows)
        set(portable_directory "AdVisual")
    else()
        message(FATAL_ERROR "Portable release packaging supports macOS arm64 and Windows x64")
    endif()
    set(ADVISUAL_PACKAGE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/release" CACHE PATH "Portable artifact directory")
    set(ADVISUAL_DEPENDENCY_SOURCE "" CACHE PATH "Source-built dependency checkouts, for license notices")
    set(ADVISUAL_DLL_DIRECTORIES "" CACHE STRING "Additional Windows runtime DLL directories")
    add_executable(advisual_portable_check packaging/relocation_probe.cpp src/common.cpp)
    target_compile_features(advisual_portable_check PRIVATE cxx_std_20)
    target_include_directories(advisual_portable_check PRIVATE src)
    target_link_libraries(advisual_portable_check PRIVATE PkgConfig::GTKMM)
    advisual_build_settings(advisual_portable_check)
    set(package_command "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/package.py" "${portable_platform}"
        --binary "$<TARGET_FILE:advisual>" --probe "$<TARGET_FILE:advisual_portable_check>"
        --source "${CMAKE_CURRENT_SOURCE_DIR}" --output "${ADVISUAL_PACKAGE_OUTPUT}"
        --prefix "${runtime_prefix}" --version "${PROJECT_VERSION}")
    if(ADVISUAL_DEPENDENCY_SOURCE)
        list(APPEND package_command --dependency-source "${ADVISUAL_DEPENDENCY_SOURCE}")
    endif()
    foreach(directory IN LISTS ADVISUAL_DLL_DIRECTORIES)
        list(APPEND package_command --dll-directory "${directory}")
    endforeach()
    add_custom_target(package-portable COMMAND ${package_command}
        DEPENDS advisual advisual_portable_check VERBATIM
        COMMENT "Bundling AdVisual, runtime libraries, resources and license notices")
    add_custom_target(check-portable
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/package.py" check
            --package "${ADVISUAL_PACKAGE_OUTPUT}/${portable_directory}"
        DEPENDS package-portable VERBATIM
        COMMENT "Checking a relocated copy with a clean runtime environment")
endif()

set(CPACK_NSIS_DEFINES " !define MUI_STARTMENUPAGE_DEFAULTFOLDER \\\"AdVisual\\\"")
include(CPack)

include(CTest)
if(BUILD_TESTING)
    set(editor_test_sources ${src_files})
    list(REMOVE_ITEM editor_test_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    add_executable(pal_editor_test tests/pal_editor_test.cpp ${editor_test_sources})
    target_compile_features(pal_editor_test PRIVATE cxx_std_20)
    target_include_directories(pal_editor_test PRIVATE src)
    advisual_build_settings(pal_editor_test)
    target_link_libraries(pal_editor_test PRIVATE PkgConfig::GTKMM adplug::adplug libbinio::libbinio)
endif()
