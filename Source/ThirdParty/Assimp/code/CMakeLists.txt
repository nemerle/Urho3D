# Listing and grouping of all the source files.
# 1) Set the file lists for each component
# 2) Create a Source Group for each component, for IDE project orginization
# 3) Add libassimp using the file lists (eliminates duplication of file names between
#    source groups and library command)
#
cmake_minimum_required( VERSION 2.8.12 )
SET( HEADER_PATH ../include/assimp )

SET( COMPILER_HEADERS
    ${HEADER_PATH}/Compiler/pushpack1.h
    ${HEADER_PATH}/Compiler/poppack1.h
    ${HEADER_PATH}/Compiler/pstdint.h
)
SOURCE_GROUP( Compiler FILES ${COMPILER_HEADERS})

SET( PUBLIC_HEADERS
    ${HEADER_PATH}/anim.h
    ${HEADER_PATH}/ai_assert.h
    ${HEADER_PATH}/camera.h
    ${HEADER_PATH}/color4.h
    ${HEADER_PATH}/color4.inl
    ${HEADER_PATH}/config.h
    ${HEADER_PATH}/defs.h
    ${HEADER_PATH}/cfileio.h
    ${HEADER_PATH}/light.h
    ${HEADER_PATH}/material.h
    ${HEADER_PATH}/material.inl
    ${HEADER_PATH}/matrix3x3.h
    ${HEADER_PATH}/matrix3x3.inl
    ${HEADER_PATH}/matrix4x4.h
    ${HEADER_PATH}/matrix4x4.inl
    ${HEADER_PATH}/mesh.h
    ${HEADER_PATH}/postprocess.h
    ${HEADER_PATH}/quaternion.h
    ${HEADER_PATH}/quaternion.inl
    ${HEADER_PATH}/scene.h
    ${HEADER_PATH}/metadata.h
    ${HEADER_PATH}/texture.h
    ${HEADER_PATH}/types.h
    ${HEADER_PATH}/vector2.h
    ${HEADER_PATH}/vector2.inl
    ${HEADER_PATH}/vector3.h
    ${HEADER_PATH}/vector3.inl
    ${HEADER_PATH}/version.h
    ${HEADER_PATH}/cimport.h
    ${HEADER_PATH}/importerdesc.h
    ${HEADER_PATH}/Importer.hpp
    ${HEADER_PATH}/DefaultLogger.hpp
    ${HEADER_PATH}/ProgressHandler.hpp
    ${HEADER_PATH}/IOStream.hpp
    ${HEADER_PATH}/IOSystem.hpp
    ${HEADER_PATH}/Logger.hpp
    ${HEADER_PATH}/LogStream.hpp
    ${HEADER_PATH}/NullLogger.hpp
    ${HEADER_PATH}/cexport.h
    ${HEADER_PATH}/Exporter.hpp
)

SET( Core_SRCS
    Assimp.cpp
)

SET( Boost_SRCS
    BoostWorkaround/boost/math/common_factor_rt.hpp
    BoostWorkaround/boost/format.hpp
)
SOURCE_GROUP(Boost FILES ${Boost_SRCS})

SET( Logging_SRCS
    ${HEADER_PATH}/DefaultLogger.hpp
    ${HEADER_PATH}/LogStream.hpp
    ${HEADER_PATH}/Logger.hpp
    ${HEADER_PATH}/NullLogger.hpp
    Win32DebugLogStream.h
    DefaultLogger.cpp
    FileLogStream.h
    StdOStreamLogStream.h
)
SOURCE_GROUP(Logging FILES ${Logging_SRCS})

SET( Common_SRCS
    fast_atof.h
    qnan.h
    BaseImporter.cpp
    BaseImporter.h
    BaseProcess.cpp
    BaseProcess.h
    Importer.h
    ScenePrivate.h
    PostStepRegistry.cpp
    ImporterRegistry.cpp
    ImporterRegistry.h
    ByteSwapper.h
    DefaultProgressHandler.h
    DefaultIOStream.cpp
    DefaultIOStream.h
    DefaultIOSystem.cpp
    DefaultIOSystem.h
    CInterfaceIOWrapper.h
    Hash.h
    Importer.cpp
    IFF.h
    MemoryIOWrapper.h
    ParsingUtils.h
    StreamReader.h
    StreamWriter.h
    StringComparison.h
    SGSpatialSort.cpp
    SGSpatialSort.h
    VertexTriangleAdjacency.cpp
    VertexTriangleAdjacency.h
    GenericProperty.h
    SpatialSort.cpp
    SpatialSort.h
    SceneCombiner.cpp
    SceneCombiner.h
    ScenePreprocessor.cpp
    ScenePreprocessor.h
    SkeletonMeshBuilder.cpp
    SkeletonMeshBuilder.h
    SplitByBoneCountProcess.cpp
    SplitByBoneCountProcess.h
    SmoothingGroups.h
    StandardShapes.cpp
    StandardShapes.h
    TargetAnimation.cpp
    TargetAnimation.h
    RemoveComments.cpp
    RemoveComments.h
    Subdivision.cpp
    Subdivision.h
    Vertex.h
    LineSplitter.h
    TinyFormatter.h
    Profiler.h
    LogAux.h
    Bitmap.cpp
    Bitmap.h
    XMLTools.h
    Version.cpp
)
SOURCE_GROUP(Common FILES ${Common_SRCS})

# macro to collect the given Loader's sources as an OBJECT library
MACRO(DEFINE_IMPORTER name)
    file(GLOB format_sources *.cpp)
    file(GLOB format_headers *.h)
    SOURCE_GROUP(${name} FILES ${format_sources} ${format_headers} ${ARGN})
    add_library(${name}_format OBJECT ${format_sources} ${format_headers} ${ARGN})
ENDMACRO()

# macro to add the CMake Option ADD_ASSIMP_IMPORTER_<name> which enables compilation of given loader
# this way selective loaders can be compiled (reduces filesize + compile time)
MACRO(ADD_ASSIMP_IMPORTER_DIR name)
    OPTION(ASSIMP_BUILD_${name}_IMPORTER "build the ${name} importer" TRUE)
    IF(ASSIMP_BUILD_${name}_IMPORTER)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/Formats/${name})
        SET(ASSIMP_IMPORTERS_ENABLED ${ASSIMP_IMPORTERS_ENABLED} ${name})
        LIST(APPEND ASSIMP_LOADER_SRCS $<TARGET_OBJECTS:${name}_format>)
    ELSE()
        SET(${name}_SRC "")
        SET(ASSIMP_IMPORTERS_DISABLED "${ASSIMP_IMPORTERS_DISABLED} ${name}")
        add_definitions(-DASSIMP_BUILD_NO_${name}_IMPORTER)
    ENDIF()
ENDMACRO()

SET(ASSIMP_LOADER_SRCS "")
SET(ASSIMP_IMPORTERS_ENABLED "") # list of enabled importers
SET(ASSIMP_IMPORTERS_DISABLED "") # disabled list (used to print)

include(Formats/AssimpFormats.cmake)

################################################################################################
# Create a static 'plugin' registration command list
set(built_string "")
foreach(z ${ASSIMP_IMPORTERS_ENABLED})
    set(built_string "${built_string} REGISTER_FORMAT(${z});\n")
endforeach()
#create registration list
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/AllPlugins.h "${built_string}" )
ADD_CUSTOM_TARGET( generate_registry DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/AllPlugins.h )
################################################################################################


SET( MaterialSystem_SRCS
    MaterialSystem.cpp
    MaterialSystem.h
)
SOURCE_GROUP( MaterialSystem FILES ${MaterialSystem_SRCS})
SET( PostProcessing_SRCS
    CalcTangentsProcess.cpp
    CalcTangentsProcess.h
    ComputeUVMappingProcess.cpp
    ComputeUVMappingProcess.h
    ConvertToLHProcess.cpp
    ConvertToLHProcess.h
    FindDegenerates.cpp
    FindDegenerates.h
    FindInstancesProcess.cpp
    FindInstancesProcess.h
    FindInvalidDataProcess.cpp
    FindInvalidDataProcess.h
    FixNormalsStep.cpp
    FixNormalsStep.h
    GenFaceNormalsProcess.cpp
    GenFaceNormalsProcess.h
    GenVertexNormalsProcess.cpp
    GenVertexNormalsProcess.h
    PretransformVertices.cpp
    PretransformVertices.h
    ImproveCacheLocality.cpp
    ImproveCacheLocality.h
    JoinVerticesProcess.cpp
    JoinVerticesProcess.h
    LimitBoneWeightsProcess.cpp
    LimitBoneWeightsProcess.h
    RemoveRedundantMaterials.cpp
    RemoveRedundantMaterials.h
    RemoveVCProcess.cpp
    RemoveVCProcess.h
    SortByPTypeProcess.cpp
    SortByPTypeProcess.h
    SplitLargeMeshes.cpp
    SplitLargeMeshes.h
    TextureTransform.cpp
    TextureTransform.h
    TriangulateProcess.cpp
    TriangulateProcess.h
    ValidateDataStructure.cpp
    ValidateDataStructure.h
    OptimizeGraph.cpp
    OptimizeGraph.h
    OptimizeMeshes.cpp
    OptimizeMeshes.h
    DeboneProcess.cpp
    DeboneProcess.h
    ProcessHelper.h
    ProcessHelper.cpp
    PolyTools.h
    MakeVerboseFormat.cpp
    MakeVerboseFormat.h
)
SOURCE_GROUP( PostProcessing FILES ${PostProcessing_SRCS})

SET( Exporter_SRCS
    Exporter.cpp
    AssimpCExport.cpp
    BlobIOSystem.h
)
SOURCE_GROUP( Exporter FILES ${Exporter_SRCS})

SET( Extra_SRCS
    MD4FileData.h
)
SOURCE_GROUP( Extra FILES ${Extra_SRCS})

SET( ConvertUTF_SRCS
    ../contrib/ConvertUTF/ConvertUTF.h
    ../contrib/ConvertUTF/ConvertUTF.c
)
SOURCE_GROUP( ConvertUTF FILES ${ConvertUTF_SRCS})

SET( Clipper_SRCS
    ../contrib/clipper/clipper.hpp
    ../contrib/clipper/clipper.cpp
)
SOURCE_GROUP( Clipper FILES ${Clipper_SRCS})


SET( Poly2Tri_SRCS
    ../contrib/poly2tri/poly2tri/common/shapes.cc
    ../contrib/poly2tri/poly2tri/common/shapes.h
    ../contrib/poly2tri/poly2tri/common/utils.h
    ../contrib/poly2tri/poly2tri/sweep/advancing_front.h
    ../contrib/poly2tri/poly2tri/sweep/advancing_front.cc
    ../contrib/poly2tri/poly2tri/sweep/cdt.cc
    ../contrib/poly2tri/poly2tri/sweep/cdt.h
    ../contrib/poly2tri/poly2tri/sweep/sweep.cc
    ../contrib/poly2tri/poly2tri/sweep/sweep.h
    ../contrib/poly2tri/poly2tri/sweep/sweep_context.cc
    ../contrib/poly2tri/poly2tri/sweep/sweep_context.h
)
SOURCE_GROUP( Poly2Tri FILES ${Poly2Tri_SRCS})

SET( unzip_SRCS
    ../contrib/unzip/crypt.h
    ../contrib/unzip/ioapi.c
    ../contrib/unzip/ioapi.h
    ../contrib/unzip/unzip.c
    ../contrib/unzip/unzip.h
)
SOURCE_GROUP( unzip FILES ${unzip_SRCS})

# VC2010 fixes
if(MSVC10)
    option( VC10_STDINT_FIX "Fix for VC10 Compiler regarding pstdint.h redefinition errors" OFF )
    if( VC10_STDINT_FIX )
        ADD_DEFINITIONS( -D_STDINT )
    endif( VC10_STDINT_FIX )
endif(MSVC10)

ADD_DEFINITIONS( -DASSIMP_BUILD_DLL_EXPORT )

if ( MSVC )
    ADD_DEFINITIONS( -D_SCL_SECURE_NO_WARNINGS )
    ADD_DEFINITIONS( -D_CRT_SECURE_NO_WARNINGS )
endif ( MSVC )

if (UNZIP_FOUND)
    SET (unzip_compile_SRCS "")
else (UNZIP_FOUND)
    SET (unzip_compile_SRCS ${unzip_SRCS})
endif (UNZIP_FOUND)

MESSAGE(STATUS "Enabled formats:${ASSIMP_IMPORTERS_ENABLED}")
MESSAGE(STATUS "Disabled formats:${ASSIMP_IMPORTERS_DISABLED}")
SET( assimp_src
    # Assimp Files
    ${Core_SRCS}
    ${Common_SRCS}
    ${Logging_SRCS}
    ${Exporter_SRCS}
    ${PostProcessing_SRCS}
    ${MaterialSystem_SRCS}
    ${Step_SRCS}

    # Model Support
    ${ASSIMP_LOADER_SRCS}

    # Third-party libraries
    ${IrrXML_SRCS}
    ${ConvertUTF_SRCS}
    ${unzip_compile_SRCS}
    ${Poly2Tri_SRCS}
    ${Clipper_SRCS}
    # Necessary to show the headers in the project when using the VC++ generator:
    ${Boost_SRCS}

    ${PUBLIC_HEADERS}
    ${COMPILER_HEADERS}

)
add_definitions( -DOPENDDLPARSER_BUILD )

INCLUDE_DIRECTORIES(
    ../contrib/openddlparser/include
    ${CMAKE_CURRENT_BINARY_DIR}  # AllPlugins.h is located here
)
ADD_LIBRARY( Assimp ${assimp_src} ${CMAKE_CURRENT_BINARY_DIR}/AllPlugins.h)

TARGET_LINK_LIBRARIES(Assimp ${ZLIB_LIBRARIES} ${OPENDDL_PARSER_LIBRARIES} )
add_dependencies(Assimp generate_registry)

if (APPLE)
    SET_TARGET_PROPERTIES( Assimp PROPERTIES INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}/${LIB_INSTALL_DIR}")
endif()

# Build against external unzip, or add ../contrib/unzip so
# assimp can #include "unzip.h"
if (UNZIP_FOUND)
    INCLUDE_DIRECTORIES(${UNZIP_INCLUDE_DIRS})
    TARGET_LINK_LIBRARIES(Assimp ${UNZIP_LIBRARIES})
else (UNZIP_FOUND)
    INCLUDE_DIRECTORIES("../contrib/unzip")
endif (UNZIP_FOUND)
