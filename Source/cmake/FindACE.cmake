# Find the ACE headers and library

SET(ACE_debug_lib FALSE CACHE BOOL "Use debug version of ACE")
SET(TAO_debug_lib FALSE CACHE BOOL "Use debug version of TAO")
SET(ACELIB_extension "")
SET(TAOLIB_extension "")



IF(CMAKE_CXX_COMPILER MATCHES cl)
        SET(ACELIB_extension "d")
        SET(TAOLIB_extension "d")
ENDIF (CMAKE_CXX_COMPILER MATCHES cl)

IF (ACE_PATH AND ACE_LIB)
    SET(ACE_FIND_QUIETLY TRUE)
ENDIF (ACE_PATH AND ACE_LIB)

FIND_PATH(ACE_PATH include/ace/ACE.h
  /usr/local/include/ace
  /usr/include/ace
  ${MAIN_INCLUDE_PATH}
  ${MAIN_INCLUDE_PATH}/ace
)
MESSAGE(STATUS ACE${ACELIB_extension}.lib)
FIND_LIBRARY(ACE_LIB
        NAMES ACE${ACELIB_extension}
        PATHS
                /usr/
                /usr/local
                ${MAIN_LIB_PATH}
                ${ACE_PATH}
                ${ACE_PATH}/lib
        PATH_SUFFIXES
            lib
        DOC "Path to ACE framework library"
)
SET(ACE_FOUND FALSE)

IF (ACE_PATH AND ACE_LIB)
   SET(ACE_FOUND TRUE)
ENDIF (ACE_PATH AND ACE_LIB)

IF (ACE_FOUND)
  IF (NOT ACE_FIND_QUIETLY)
    MESSAGE(STATUS "Found ACE: ${ACE_LIB}")
  ENDIF (NOT ACE_FIND_QUIETLY)
ELSE (ACE_FOUND)
  IF (ACE_FIND_REQUIRED)
    MESSAGE(FATAL_ERROR "Could NOT find ACE")
  ENDIF (ACE_FIND_REQUIRED)
ENDIF (ACE_FOUND)

MARK_AS_ADVANCED(ACE_LIB)

FUNCTION(ACE_ADD_LIBRARIES target)
        FOREACH(libname ${ARGN})
                TARGET_LINK_LIBRARIES(${target} debug ${libname}${ACELIB_extension}
                                                optimized ${libname})
        ENDFOREACH()
ENDFUNCTION()

