# Install headers for building the Urho3D library
install_header_files (DIRECTORY ./ DESTINATION ${DEST_INCLUDE_DIR}/ThirdParty/sherwood_map FILES_MATCHING PATTERN *.hpp BUILD_TREE_ONLY)  # Note: the trailing slash is significant
