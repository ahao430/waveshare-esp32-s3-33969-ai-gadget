# cJSON v1.7.18 本地组件（IDF v6 已移除内置 cjson；源码与模拟器
# FetchContent 同版本，自 simulator/lvgl-pc/build/_deps/cjson-src 复制）。
idf_component_register(SRCS "cJSON.c" INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
