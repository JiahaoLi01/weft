# The engine bridge's values and queues (docs/engine-communication.md, section 6): no script engine, no platform.
weft_add_library(bridge
    SOURCES bridge_value.cpp engine_channel.cpp
    PUBLIC weft::base)
