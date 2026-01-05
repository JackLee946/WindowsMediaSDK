set(DEMO_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/minidump.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/minidump.h
    ${CMAKE_CURRENT_SOURCE_DIR}/main_window.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/main_window.h
    ${CMAKE_CURRENT_SOURCE_DIR}/my_window.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/my_window.h
    ${CMAKE_CURRENT_SOURCE_DIR}/video_device_choose_window.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/video_device_choose_window.h
)

source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${DEMO_SOURCE})

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../mediasdk)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../mediasdk/common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../mediasdk/camera_capture)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../mediasdk/video_render)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../mediasdk/video_encoder)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../mediasdk/audio_capture)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../mediasdk/local_log)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/include/rtmp)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/include/ffmpeg)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../DuiLib)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/detours)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/opengl)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/openh264)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/sdl2)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/x264)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/${Configuration}/ffmpeg)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/${Configuration}/mfx)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/${Configuration}/x265)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/${Configuration}/yuv)
link_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/lib/${Configuration}/jpeg-turbo)

add_executable(rtmp_push_demo ${DEMO_SOURCE})
target_link_libraries(rtmp_push_demo mediasdk duilib libeasyrtmp avutil swresample dbghelp)

set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT rtmp_push_demo)

add_custom_command(TARGET rtmp_push_demo
                   POST_BUILD
                   COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../script/copy_file.bat
                   COMMENT "copy file")

# Copy XML files to output directory
add_custom_command(TARGET rtmp_push_demo
                   POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E make_directory
                     $<TARGET_FILE_DIR:rtmp_push_demo>/xml
                   COMMENT "create xml directory")

add_custom_command(TARGET rtmp_push_demo
                   POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     ${CMAKE_CURRENT_SOURCE_DIR}/xml/main_window.xml
                     $<TARGET_FILE_DIR:rtmp_push_demo>/xml/main_window.xml
                   COMMENT "copy main_window.xml")

add_custom_command(TARGET rtmp_push_demo
                   POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     ${CMAKE_CURRENT_SOURCE_DIR}/xml/video_device_window.xml
                     $<TARGET_FILE_DIR:rtmp_push_demo>/xml/video_device_window.xml
                   COMMENT "copy video_device_window.xml")

# libeasyrtmp is built from source as a static library (no dll copy needed)



