# C ABI runtime behind include/weft/weft.h; owner_executor.hpp is header-only.
weft_add_library(core SOURCES runtime.cpp PUBLIC weft::base)
