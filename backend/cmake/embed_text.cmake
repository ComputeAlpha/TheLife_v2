# Embeds a text file as a C++ string: cmake -DIN=file -DOUT=file.cc -DSYMBOL=name -P embed_text.cmake
file(READ "${IN}" content)
string(FIND "${content}" ")TLEMBED\"" clash)
if(NOT clash EQUAL -1)
  message(FATAL_ERROR "${IN} contains the raw-string delimiter")
endif()
file(WRITE "${OUT}" "// GENERATED from ${IN}. Do not edit.\n#include <string_view>\nnamespace thelife::generated {\nextern const std::string_view ${SYMBOL};\nconst std::string_view ${SYMBOL} = R\"TLEMBED(${content})TLEMBED\";\n}\n")
