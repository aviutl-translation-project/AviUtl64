cmake_minimum_required(VERSION 3.15)
project(reaviutl LANGUAGES C)

# c standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	add_compile_definitions(REAVIUTL_X64=1)
else()
	add_compile_definitions(REAVIUTL_X64=0)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

# core sources shared with 32-bit bridge
set(REAVIUTL_CORE_SOURCES
	src/plugin_manager.c
	src/plugin_loader.c
	src/plugin_bridge_client.c
	src/video_export.c
	src/aup_project.c
	src/debug_console.c
	src/exfunc.c
	src/editp.c
	src/yc48_convert.c
	src/vfw_input.c
)

# main executable
add_executable(reaviutl
	src/gui_main.c
	src/preview.c
	src/i18n.c
	src/aviutl_menu.c
	src/aviutl_resources.rc
	${REAVIUTL_CORE_SOURCES}
)

# compiler and linker flags
if(MSVC)
	target_compile_options(reaviutl
		PRIVATE
			/W3
			/O2
			/wd4819
			/wd5105
			/D_CRT_SECURE_NO_WARNINGS
	)

	target_link_options(reaviutl
		PRIVATE
			/SUBSYSTEM:CONSOLE
			/ENTRY:WinMainCRTStartup
	)
else()
	target_compile_options(reaviutl
		PRIVATE
			-Wall
			-O2
			-D_CRT_SECURE_NO_WARNINGS
	)
endif()

# link with required windows libraries
target_link_libraries(reaviutl
	PRIVATE
		user32
		gdi32
		comdlg32
		comctl32
		shell32
		vfw32
		msacm32
		winmm
)

# 32-bit plugin bridge helper (built on x86)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
	add_executable(reaviutl_bridge32
		src/bridge32.c
		${REAVIUTL_CORE_SOURCES}
	)

	if(MSVC)
		target_compile_options(reaviutl_bridge32
			PRIVATE
				/W3
				/O2
				/wd4819
				/wd5105
				/D_CRT_SECURE_NO_WARNINGS
		)
	endif()

	target_link_libraries(reaviutl_bridge32
		PRIVATE
			user32
			gdi32
			comdlg32
			comctl32
			shell32
			vfw32
			msacm32
			winmm
	)
endif()

# print build information
message(STATUS "Building reaviutl")
message(STATUS "Pointer size: ${CMAKE_SIZEOF_VOID_P}")
