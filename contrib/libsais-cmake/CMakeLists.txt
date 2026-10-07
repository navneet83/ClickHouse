# libsais: linear-time suffix array construction, used by the substring index of the text skip index.
# Only the 32-bit variant is built; the index addresses its text with 32-bit offsets.

set (LIBRARY_DIR "${ClickHouse_SOURCE_DIR}/contrib/libsais")

add_library (_libsais "${LIBRARY_DIR}/src/libsais.c")
add_library (ch_contrib::libsais ALIAS _libsais)

target_include_directories (_libsais SYSTEM PUBLIC "${LIBRARY_DIR}/include")
