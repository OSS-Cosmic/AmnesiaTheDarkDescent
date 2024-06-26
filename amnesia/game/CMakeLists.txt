cmake_minimum_required (VERSION 3.0)
project(Amnesia)

file(GLOB all_sources RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    *.cpp
    *.h
)
file(GLOB serial_source RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    LuxCopyProtection.cpp
    LuxKeyEntry.cpp
    ../serial_gen/SerialGenAlgo.cpp
)
list(REMOVE_ITEM all_sources ${serial_source})

set(compile_defs "")
set(demo_compile_defs "LUX_DEMO_VERSION")

option(USE_GAMEPAD "Enable gamepad support" ON)
if(USE_GAMEPAD)
	list(APPEND compile_defs USE_GAMEPAD)
	list(APPEND demo_compile_defs USE_GAMEPAD)
endif()

option(AMNESIA_WITH_SERIAL "Enable Serial Key Checking" OFF)
if(AMNESIA_WITH_SERIAL)
	list(APPEND compile_defs COPY_PROTECTION_ENABLED)
	set(full_sources ${all_sources} ${serial_source})
else()
	set(full_sources ${all_sources})
endif()


add_executable(Amnesia MACOSX_BUNDLE
    ${full_sources}
)

target_link_libraries(Amnesia HPL2)
set_target_properties(Amnesia PROPERTIES
  COMPILE_DEFINITIONS "${compile_defs}"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Game-Info.plist"
  MACOSX_BUNDLE_ICON_FILE "Lux.icns"
  MACOSX_BUNDLE_BUNDLE_VERSION ${VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${VERSION}
  MACOSX_BUNDLE_GUI_IDENTIFIER "com.frictionalgames.Amnesia"
  MACOSX_BUNDLE_DISPLAY_NAME "Amnesia"
  MACOSX_BUNDLE_BUNDLE_NAME "Amnesia: The Dark Descent"
)

IF(APPLE)
add_definitions(
    -DMAC_OS
)
ELSEIF(LINUX)
add_definitions(
    -DLINUX
)
ENDIF()

add_executable(AmnesiaDemo MACOSX_BUNDLE EXCLUDE_FROM_ALL
    ${all_sources}
)

target_link_libraries(AmnesiaDemo HPL2)
set_target_properties(AmnesiaDemo PROPERTIES
  COMPILE_DEFINITIONS "${demo_compile_defs}"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Game-Info.plist"
  MACOSX_BUNDLE_ICON_FILE "Lux.icns"
  MACOSX_BUNDLE_BUNDLE_VERSION ${VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${VERSION}-demo"
  MACOSX_BUNDLE_GUI_IDENTIFIER "com.frictionalgames.AmnesiaDemo"
  MACOSX_BUNDLE_BUNDLE_NAME "Amnesia: The Dark Descent"
)

IF(APPLE)
  PostBuildMacBundle(Amnesia "${HPL2_FRAMEWORKS}" "${HPL2_LIBRARIES}")
  PostBuildMacBundle(AmnesiaDemo "${HPL2_FRAMEWORKS}" "${HPL2_LIBRARIES}")
  install(TARGETS Amnesia
    RUNTIME DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/../../redist"
    BUNDLE DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/../../redist"
    )
ENDIF()

