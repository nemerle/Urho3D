# Install headers for building the Urho3D library

# Define target name
set (TARGET_NAME sherwood_map)

define_source_files (GLOB_CPP_PATTERNS src/*.cpp GLOB_H_PATTERNS include/*.hpp)

# Define dependency libs
set (INCLUDE_DIRS include)

# Setup target
setup_library ()

# Install headers for and using Urho3D
install_header_files (DIRECTORY include/ DESTINATION ${DEST_INCLUDE_DIR}/ThirdParty/sherwood_map FILES_MATCHING PATTERN *.hpp) # Note: the trailing slash is significant