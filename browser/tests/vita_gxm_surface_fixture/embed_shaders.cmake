# cmake -DOUTPUT=<header> -DSHADER_DIR=<dir> -DSHADERS=<a;b;...> -P embed_shaders.cmake
# One aligned byte array per GXP program, named after its file.
set(_text "")
foreach(shader ${SHADERS})
    file(READ "${SHADER_DIR}/${shader}.gxp" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    string(APPEND _text "static const unsigned char ${shader}[] __attribute__((aligned(16))) = {${_bytes}};\n")
endforeach()
file(WRITE "${OUTPUT}" "${_text}")
