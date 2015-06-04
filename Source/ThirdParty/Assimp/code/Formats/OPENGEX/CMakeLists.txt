include_directories(../..)
include_directories(openddlparser/include)
#add_subdirectory(openddlparser)
SET ( openddl_parser_SRCS
    openddlparser/code/OpenDDLParser.cpp
    openddlparser/code/DDLNode.cpp
    openddlparser/code/Value.cpp
    openddlparser/include/openddlparser/OpenDDLParser.h
    openddlparser/include/openddlparser/OpenDDLParserUtils.h
    openddlparser/include/openddlparser/OpenDDLCommon.h
    openddlparser/include/openddlparser/DDLNode.h
    openddlparser/include/openddlparser/Value.h
)
SOURCE_GROUP( openddl_parser FILES ${openddl_parser_SRCS})

# extract the full directory path to this CMakeLists.txt
get_filename_component(my_full_dir ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt DIRECTORY)
# treat the received path as a file name, and extract this directory name
get_filename_component(my_dirname ${my_full_dir} NAME)
DEFINE_IMPORTER(${my_dirname} ${openddl_parser_SRCS})
