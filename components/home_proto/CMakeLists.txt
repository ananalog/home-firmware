# The generated protocol codec lives in the home-protocol submodule.
set(proto_dir ${CMAKE_CURRENT_LIST_DIR}/../../external/home-protocol/c)
idf_component_register(SRCS "${proto_dir}/src/home_tlv.c" "${proto_dir}/src/home_proto.c"
                       INCLUDE_DIRS "${proto_dir}/include")
