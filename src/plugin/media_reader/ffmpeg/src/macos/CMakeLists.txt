set(TARGET_NAME media_reader_ffmpeg_macos)

add_library(${TARGET_NAME} STATIC
	ffmpeg_stream_videotoolbox.cpp
	vt_decoder.cpp
)
add_library(xstudio::media_reader::ffmpeg_macos ALIAS ${TARGET_NAME})

default_plugin_options(${TARGET_NAME})

target_compile_definitions(${TARGET_NAME} PRIVATE XSTUDIO_HAS_VIDEOTOOLBOX=1)
target_include_directories(${TARGET_NAME}
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/..
		${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(${TARGET_NAME}
	PRIVATE
		xstudio::media_reader
		${FFMPEG_LIBRARIES}
		"-framework VideoToolbox"
		"-framework CoreMedia"
		"-framework CoreVideo"
		"-framework CoreFoundation"
)

if(UNIX)
	target_compile_options(${TARGET_NAME} PRIVATE -Wfatal-errors)
endif()

if(NOT ${USE_VCPKG})
	target_link_libraries(${TARGET_NAME}
		PRIVATE
			FFMPEG::avcodec
			FFMPEG::avformat
			FFMPEG::swscale
			FFMPEG::avutil
	)
endif()
