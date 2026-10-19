# Writes a text file into a C++ include as bytes: cmake -DIN=<file> -DOUT=<include> -DNAME=<name> -P this.
# A missing file gives an empty array. Used for translations/pt_BR.txt (src/cott_language.cpp).
if(EXISTS "${IN}")
    file(READ "${IN}" hex HEX)
else()
    set(hex "")
endif()
string(LENGTH "${hex}" length)
math(EXPR size "${length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "// Made from ${IN} by app/cmake/embed_text.cmake.\nconstexpr unsigned char ${NAME}[] = {${bytes}0};\nconstexpr unsigned long ${NAME}Size = ${size};\n")
