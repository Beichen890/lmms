IF(NOT LMMS_BUILD_LINUX AND NOT LMMS_BUILD_WIN32)
	MESSAGE(STATUS "VST3 plug-in hosting is only available on Linux and Windows - skipping")
	RETURN()
ENDIF()

SET(VST3SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk")
IF(NOT EXISTS "${VST3SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp")
	MESSAGE(STATUS "VST3 SDK submodule not found - VST3 plug-in support disabled")
	RETURN()
ENDIF()

SET(LMMS_HAVE_VST3 TRUE CACHE INTERNAL "VST3 plug-in hosting is available")

INCLUDE(BuildPlugin)

SET(VST3SDK_SOURCES
	${VST3SDK_ROOT}/base/source/baseiids.cpp
	${VST3SDK_ROOT}/base/source/fbuffer.cpp
	${VST3SDK_ROOT}/base/source/fdebug.cpp
	${VST3SDK_ROOT}/base/source/fdynlib.cpp
	${VST3SDK_ROOT}/base/source/fobject.cpp
	${VST3SDK_ROOT}/base/source/fstreamer.cpp
	${VST3SDK_ROOT}/base/source/fstring.cpp
	${VST3SDK_ROOT}/base/source/timer.cpp
	${VST3SDK_ROOT}/base/source/updatehandler.cpp
	${VST3SDK_ROOT}/base/thread/source/fcondition.cpp
	${VST3SDK_ROOT}/base/thread/source/flock.cpp
	${VST3SDK_ROOT}/pluginterfaces/base/conststringtable.cpp
	${VST3SDK_ROOT}/pluginterfaces/base/coreiids.cpp
	${VST3SDK_ROOT}/pluginterfaces/base/funknown.cpp
	${VST3SDK_ROOT}/pluginterfaces/base/ustring.cpp
	${VST3SDK_ROOT}/public.sdk/source/common/commoniids.cpp
	${VST3SDK_ROOT}/public.sdk/source/common/commonstringconvert.cpp
	${VST3SDK_ROOT}/public.sdk/source/common/memorystream.cpp
	${VST3SDK_ROOT}/public.sdk/source/common/openurl.cpp
	${VST3SDK_ROOT}/public.sdk/source/common/readfile.cpp
	${VST3SDK_ROOT}/public.sdk/source/vst/hosting/connectionproxy.cpp
	${VST3SDK_ROOT}/public.sdk/source/vst/hosting/eventlist.cpp
	${VST3SDK_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp
	${VST3SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp
	${VST3SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp
	${VST3SDK_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
	${VST3SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp
	${VST3SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp
	${VST3SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
)

IF(LMMS_BUILD_WIN32)
	LIST(APPEND VST3SDK_SOURCES
		${VST3SDK_ROOT}/public.sdk/source/common/systemclipboard_win32.cpp
		${VST3SDK_ROOT}/public.sdk/source/common/threadchecker_win32.cpp
		${VST3SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp
	)
ELSE()
	LIST(APPEND VST3SDK_SOURCES
		${VST3SDK_ROOT}/public.sdk/source/common/systemclipboard_linux.cpp
		${VST3SDK_ROOT}/public.sdk/source/common/threadchecker_linux.cpp
		${VST3SDK_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp
	)
ENDIF()

BUILD_PLUGIN(vst3base
	Vst3Host.cpp
	Vst3SubPluginFeatures.cpp
	vst3base.cpp
	${VST3SDK_SOURCES}
	MOCFILES Vst3SubPluginFeatures.h
	EXPORT_BASE_NAME vst3base
	LINK SHARED
)

TARGET_INCLUDE_DIRECTORIES(vst3base
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${VST3SDK_ROOT}"
)

TARGET_COMPILE_FEATURES(vst3base PUBLIC cxx_std_17)

# Third party SDK code is not written against LMMS' warning settings.
IF(MSVC)
	SET_SOURCE_FILES_PROPERTIES(${VST3SDK_SOURCES}
		PROPERTIES COMPILE_FLAGS "/w")
ELSE()
	SET_SOURCE_FILES_PROPERTIES(${VST3SDK_SOURCES}
		PROPERTIES COMPILE_FLAGS "-w")
ENDIF()

IF(LMMS_BUILD_WIN32)
	TARGET_LINK_LIBRARIES(vst3base shell32 ole32 uuid)
ELSE()
	TARGET_LINK_LIBRARIES(vst3base ${CMAKE_DL_LIBS} pthread)
ENDIF()

# The VST3 SDK is MIT licensed and its license has to be shipped with binaries.
INSTALL(FILES "${VST3SDK_ROOT}/LICENSE.txt"
	DESTINATION "${PLUGIN_DIR}/Vst3"
	RENAME "VST3SDK-LICENSE.txt"
)
