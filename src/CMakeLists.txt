cmake_minimum_required(VERSION 3.8 FATAL_ERROR)

find_program(CLANG_FORMAT_EXE NAMES "clang-format" DOC "Path to clang-format executable")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

set(DD_SOURCES ${PROJECT_SOURCE_DIR}/src/main.cpp)

file(GLOB_RECURSE SHADER_SOURCES ${PROJECT_SOURCE_DIR}/src/*.glsl)

if(APPLE)
    add_executable(DeferredDecals MACOSX_BUNDLE ${DD_SOURCES} ${SHADER_SOURCES} ${ASSET_SOURCES})
    set(MACOSX_BUNDLE_BUNDLE_NAME "Deferred Decals") 
    set_source_files_properties(${SHADER_SOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/shader)
    set_source_files_properties(${ASSET_SOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
else()
    add_executable(DeferredDecals ${DD_SOURCES}) 
endif()

target_link_libraries(DeferredDecals dwSampleFramework)
target_link_libraries(DeferredDecals embree)

if (NOT APPLE)
    add_custom_command(TARGET DeferredDecals POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/src/shader $<TARGET_FILE_DIR:DeferredDecals>/shader)
    add_custom_command(TARGET DeferredDecals POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data/mesh $<TARGET_FILE_DIR:DeferredDecals>/mesh)
    add_custom_command(TARGET DeferredDecals POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data/texture $<TARGET_FILE_DIR:DeferredDecals>/texture)
endif()

if(CLANG_FORMAT_EXE)
    add_custom_target(DeferredDecals-clang-format COMMAND ${CLANG_FORMAT_EXE} -i -style=file ${DD_SOURCES} ${SHADER_SOURCES})
endif()

set_property(TARGET DeferredDecals PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/$(Configuration)")