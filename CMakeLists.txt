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

set_target_properties(ModelBrowser PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
