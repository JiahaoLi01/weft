# The native DOM tree, DOM values, the shared selector engine, node side tables and attribute values.
weft_add_library(dom SOURCES selector.cpp attributes.cpp tree.cpp snapshot.cpp PUBLIC weft::base)
