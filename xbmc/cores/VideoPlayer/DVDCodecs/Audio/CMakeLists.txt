set(SOURCES DVDAudioCodecFFmpeg.cpp
            DVDAudioCodecOmniphony.cpp
            DVDAudioCodecPassthrough.cpp
            OmniphonyPcmSource.cpp
            OmniphonyPcmStream.cpp)

set(HEADERS DVDAudioCodec.h
            DVDAudioCodecFFmpeg.h
            DVDAudioCodecOmniphony.h
            DVDAudioCodecPassthrough.h
            OmniphonyPcmSource.h
            OmniphonyPcmStream.h)

if(CORE_SYSTEM_NAME STREQUAL android)
  list(APPEND SOURCES DVDAudioCodecAndroidMediaCodec.cpp)
  list(APPEND HEADERS DVDAudioCodecAndroidMediaCodec.h)
endif()

core_add_library(dvdaudiocodecs)
