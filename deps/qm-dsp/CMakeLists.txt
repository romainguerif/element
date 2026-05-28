# SPDX-FileCopyrightText: 2026 Kushview, LLC
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Minimal CMake build for the Queen Mary DSP library, scoped to the
# tempo-tracking subset (DetectionFunction + TempoTrackV2 + DownBeat).
# Element only needs offline BPM/beat analysis.

add_library(qm-dsp STATIC
    base/KaiserWindow.cpp
    base/Pitch.cpp
    base/SincWindow.cpp
    maths/Correlation.cpp
    maths/MathUtilities.cpp
    dsp/transforms/FFT.cpp
    dsp/phasevocoder/PhaseVocoder.cpp
    dsp/rateconversion/Decimator.cpp
    dsp/rateconversion/DecimatorB.cpp
    dsp/signalconditioning/DFProcess.cpp
    dsp/signalconditioning/Filter.cpp
    dsp/signalconditioning/FiltFilt.cpp
    dsp/signalconditioning/Framer.cpp
    dsp/onsets/DetectionFunction.cpp
    dsp/onsets/PeakPicking.cpp
    dsp/tempotracking/TempoTrack.cpp
    dsp/tempotracking/TempoTrackV2.cpp
    dsp/tempotracking/DownBeat.cpp
    ext/kissfft/kiss_fft.c
    ext/kissfft/tools/kiss_fftr.c
)

target_include_directories(qm-dsp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ext/kissfft
        ${CMAKE_CURRENT_SOURCE_DIR}/ext/kissfft/tools
)

target_compile_definitions(qm-dsp
    PUBLIC
        kiss_fft_scalar=double
)

set_target_properties(qm-dsp PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(qm-dsp PRIVATE
        -Wno-unused-but-set-variable
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-unused-parameter
        -Wno-deprecated-declarations
        -Wno-sign-conversion
        -Wno-sign-compare
        -Wno-implicit-fallthrough
    )
endif()
