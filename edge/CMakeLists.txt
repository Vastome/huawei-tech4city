cmake_minimum_required(VERSION 3.20)
project(vastome_edge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs video videoio)
if(OpenCV_VERSION VERSION_GREATER_EQUAL 5)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs features video videoio)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(Tesseract REQUIRED IMPORTED_TARGET tesseract)

add_library(vastome_edge
    src/accuracy.cpp
    src/braille.cpp
    src/image_pipeline.cpp
    src/line_motion.cpp
    src/protocol.cpp
    src/reading_cursor.cpp
    src/simulator.cpp
    src/synthetic.cpp
)
target_include_directories(vastome_edge PUBLIC include)
target_link_libraries(vastome_edge PUBLIC ${OpenCV_LIBS} PkgConfig::Tesseract)
target_compile_options(vastome_edge PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(vastome_edge_sim apps/edge_sim.cpp)
target_link_libraries(vastome_edge_sim PRIVATE vastome_edge)

add_executable(vastome_accuracy_benchmark apps/accuracy_benchmark.cpp)
target_link_libraries(vastome_accuracy_benchmark PRIVATE vastome_edge)

add_executable(vastome_dataset_benchmark apps/dataset_benchmark.cpp)
target_link_libraries(vastome_dataset_benchmark PRIVATE vastome_edge)

add_executable(vastome_position_sim apps/position_sim.cpp)
target_link_libraries(vastome_position_sim PRIVATE vastome_edge)
configure_file(assets/position_viewer.html position_viewer.html COPYONLY)

enable_testing()
add_executable(vastome_edge_tests tests/edge_tests.cpp)
target_link_libraries(vastome_edge_tests PRIVATE vastome_edge)
add_test(NAME vastome_edge_tests COMMAND vastome_edge_tests)
