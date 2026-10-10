# One static library per module, aliased weft::<module>. Windows reference rasters are separate *_d2d targets.
# constants/ is header-only and reached through the shared src/ include root.
add_subdirectory(base)
add_subdirectory(core)
add_subdirectory(render)
add_subdirectory(canvas)
add_subdirectory(script)
add_subdirectory(text)
add_subdirectory(style)
add_subdirectory(layout)
add_subdirectory(scene)
add_subdirectory(input)
