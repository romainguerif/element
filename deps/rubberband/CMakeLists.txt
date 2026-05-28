# SPDX-FileCopyrightText: 2026 Kushview, LLC
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Element's minimal CMake build for Rubber Band, using the
# single-file translation unit (single/RubberBandSingle.cpp).
# On Apple platforms vDSP from Accelerate is used for FFT.

add_library(rubberband STATIC
    single/RubberBandSingle.cpp
)

target_include_directories(rubberband
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(rubberband PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(APPLE)
    target_link_libraries(rubberband PUBLIC "-framework Accelerate")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(rubberband PRIVATE
        -Wno-unused-but-set-variable
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-unused-parameter
        -Wno-deprecated-declarations
        -Wno-sign-conversion
        -Wno-sign-compare
    )
endif()
