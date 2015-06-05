cmake_minimum_required( VERSION 2.6 )

INCLUDE_DIRECTORIES(
	${Assimp_SOURCE_DIR}/include
	${Assimp_SOURCE_DIR}/code
)

LINK_DIRECTORIES( ${Assimp_BINARY_DIR} ${Assimp_BINARY_DIR}/lib )

ADD_EXECUTABLE( assimp_cmd
	assimp_cmd.rc
	CompareDump.cpp
	ImageExtractor.cpp
	Main.cpp
	Main.h
	resource.h
	WriteDumb.cpp
	Info.cpp
	Export.cpp
)

SET_PROPERTY(TARGET assimp_cmd PROPERTY DEBUG_POSTFIX ${ASSIMP_DEBUG_POSTFIX})

TARGET_LINK_LIBRARIES( assimp_cmd Assimp ${ZLIB_LIBRARIES} )
SET_TARGET_PROPERTIES( assimp_cmd PROPERTIES
	OUTPUT_NAME assimp
)

