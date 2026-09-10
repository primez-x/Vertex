set(_sketch_planegcs_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(_sketch_planegcs_vendor "${_sketch_planegcs_root}/third_party/planegcs/upstream")
set(_sketch_native_prefix "${_sketch_planegcs_root}/.deps/native/x64-windows"
    CACHE PATH "Prepared native dependency prefix for PlaneGCS")

find_package(Eigen3 5.0.1 CONFIG REQUIRED
    PATHS "${_sketch_native_prefix}/share/eigen3"
    NO_DEFAULT_PATH)

set(_sketch_planegcs_sources
    "${_sketch_planegcs_vendor}/src/Mod/Sketcher/App/planegcs/GCS.cpp"
    "${_sketch_planegcs_vendor}/src/Mod/Sketcher/App/planegcs/Geo.cpp"
    "${_sketch_planegcs_vendor}/src/Mod/Sketcher/App/planegcs/Constraints.cpp"
    "${_sketch_planegcs_vendor}/src/Mod/Sketcher/App/planegcs/SubSystem.cpp"
    "${_sketch_planegcs_vendor}/src/Mod/Sketcher/App/planegcs/qp_eq.cpp")

add_library(property_planegcs SHARED ${_sketch_planegcs_sources})
target_compile_features(property_planegcs PUBLIC cxx_std_20)
target_compile_definitions(property_planegcs
    PRIVATE PROPERTY_PLANEGCS_BUILD EIGEN_NO_DEBUG
    PUBLIC EIGEN_MPL2_ONLY)
target_include_directories(property_planegcs
    PUBLIC
        "${_sketch_planegcs_vendor}/src"
        "${_sketch_planegcs_vendor}/src/Mod/Sketcher/App/planegcs"
        "${_sketch_native_prefix}/include")
target_link_libraries(property_planegcs PUBLIC Eigen3::Eigen)
set_target_properties(property_planegcs PROPERTIES
    OUTPUT_NAME property_planegcs
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)

add_library(sketch_constraints STATIC "${_sketch_planegcs_root}/src/core/constraints.cpp")
target_compile_features(sketch_constraints PUBLIC cxx_std_20)
target_include_directories(sketch_constraints PUBLIC "${_sketch_planegcs_root}/include")
target_link_libraries(sketch_constraints PRIVATE property_planegcs)

if(MSVC)
    target_compile_options(property_planegcs PRIVATE /W3 /permissive- /utf-8)
    target_compile_options(sketch_constraints PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(property_planegcs PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(sketch_constraints PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING AND EXISTS "${_sketch_planegcs_root}/tests/constraints_tests.cpp")
    add_executable(constraints_tests "${_sketch_planegcs_root}/tests/constraints_tests.cpp")
    target_link_libraries(constraints_tests PRIVATE sketch_constraints)
    add_test(NAME constraints COMMAND constraints_tests)
endif()
