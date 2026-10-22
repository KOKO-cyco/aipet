##
# @file CMakeLists.txt
# @brief Build rules for the ai_pet application.
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

if (CONFIG_LVGL_PC_SIMULATOR STREQUAL "y")
########################################
# Simulator mode
########################################
# The PC simulator does not build the app through this file: it globs
# apps/tuya.ai/ai_pet/src from examples/graphics/lvgl_simulator/CMakeLists.txt,
# driven by sim_config.cmake. This target only has to exist so the top-level
# build has something to link.
add_library(${EXAMPLE_LIB})

target_compile_definitions(${EXAMPLE_LIB}
    PRIVATE
        LVGL_PC_SIMULATOR
)

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_PATH}/include
)

else()
########################################
# Embedded mode
########################################

# GLOB_RECURSE rather than aux_source_directory: the sources live in src/,
# src/port/ and src/fonts/, and all three have to be compiled in.
file(GLOB_RECURSE APP_SRCS
    ${APP_PATH}/src/*.c
)

# src/fonts is on the include path as well as in the glob: it holds the
# FONT_AWESOME_* glyph macros that include/pet_theme.h pulls in, and a quoted
# include resolves against the *including* file's directory, which is include/.
set(APP_INC
    ${APP_PATH}/include
    ${APP_PATH}/src/fonts
)

add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )

target_compile_options(${EXAMPLE_LIB}
    PRIVATE
        "-DLV_LVGL_H_INCLUDE_SIMPLE"
)

# The to-do cap is a Kconfig option; pass it through as a plain macro so
# pet_types.h does not have to include the generated config header.
if (CONFIG_APP_PET_TODO_MAX)
    target_compile_definitions(${EXAMPLE_LIB}
        PRIVATE
            PET_TODO_MAX=${CONFIG_APP_PET_TODO_MAX}
    )
endif()

endif()
