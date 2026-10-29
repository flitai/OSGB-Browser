cmake_minimum_required(VERSION 3.16)
project(ModelBrowser)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSceneGraph REQUIRED
    COMPONENTS
        osg
        osgDB
        osgGA
        osgViewer
        osgText
)

add_executable(ModelBrowser ModelBrowser.cpp)

target_include_directories(ModelBrowser PRIVATE
    ${OPENSCENEGRAPH_INCLUDE_DIRS}
)

target_link_libraries(ModelBrowser PRIVATE
    ${OPENSCENEGRAPH_LIBRARIES}
)

if(APPLE)
    target_link_libraries(ModelBrowser PRIVATE
        "-framework Cocoa"
    )
endif()

if(WIN32)
    # GUI 程序（无控制台窗口），仍以 main() 为入口
    set_target_properties(ModelBrowser PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
        target_link_options(ModelBrowser PRIVATE /ENTRY:mainCRTStartup)
        target_compile_options(ModelBrowser PRIVATE /utf-8)
    endif()
    target_compile_definitions(ModelBrowser PRIVATE NOMINMAX UNICODE _UNICODE)
    target_link_libraries(ModelBrowser PRIVATE ole32 shell32 uuid)
endif()

set_target_properties(ModelBrowser PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
