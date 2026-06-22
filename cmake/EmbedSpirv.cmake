if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "EmbedSpirv requires INPUT, OUTPUT, and SYMBOL")
endif()

file(READ "${INPUT}" VF_SPIRV_HEX HEX)
string(LENGTH "${VF_SPIRV_HEX}" VF_SPIRV_HEX_LENGTH)
math(EXPR VF_SPIRV_BYTES "${VF_SPIRV_HEX_LENGTH} / 2")
math(EXPR VF_SPIRV_REMAINDER "${VF_SPIRV_BYTES} % 4")
if(NOT VF_SPIRV_REMAINDER EQUAL 0)
    message(FATAL_ERROR "SPIR-V byte count must be divisible by four")
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," VF_SPIRV_VALUES "${VF_SPIRV_HEX}")
file(WRITE "${OUTPUT}"
    "#pragma once\n#include <cstddef>\n#include <cstdint>\n\n"
    "namespace vf::renderer::backend::shaders {\n"
    "alignas(4) inline constexpr std::uint8_t ${SYMBOL}[] = {${VF_SPIRV_VALUES}};\n"
    "inline constexpr std::size_t ${SYMBOL}Size = sizeof(${SYMBOL});\n"
    "}\n")
