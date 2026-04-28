pybind11_add_module(_asclepius_py asclepius_py.cpp)

target_link_libraries(_asclepius_py
    PRIVATE
        asclepius::asclepius
)

set_target_properties(_asclepius_py PROPERTIES
    OUTPUT_NAME "_asclepius"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/asclepius"
)

# Stage the Python package layout next to the extension so callers can do
# `PYTHONPATH=build/bindings/python python -c "import asclepius"`.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/asclepius/__init__.py
               ${CMAKE_CURRENT_BINARY_DIR}/asclepius/__init__.py
               COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/asclepius/policies.py
               ${CMAKE_CURRENT_BINARY_DIR}/asclepius/policies.py
               COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/asclepius/bench.py
               ${CMAKE_CURRENT_BINARY_DIR}/asclepius/bench.py
               COPYONLY)
