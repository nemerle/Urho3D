include_directories(../..)
SET(IrrXML_SRCS
    irrXMLWrapper.h
    irrXML/CXMLReaderImpl.h
    irrXML/heapsort.h
    irrXML/irrArray.h
    irrXML/irrString.h
    irrXML/irrTypes.h
    irrXML/irrXML.cpp
    irrXML/irrXML.h
)
# extract the full directory path to this CMakeLists.txt
get_filename_component(my_full_dir ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt DIRECTORY)
# treat the received path as a file name, and extract this directory name
get_filename_component(my_dirname ${my_full_dir} NAME)
DEFINE_IMPORTER(${my_dirname} ${IrrXML_SRCS})
