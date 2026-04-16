set(srcs
    "main.c"
    "state_machine.c"
    "usb_msc.c"
    "sd_card.c"
    "sync.c"
    "app_mqtt.c"
    "nvs_config.c"
    "led.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES fatfs nvs_flash esp_wifi mqtt esp_http_client json driver
)

# Parse .env file at build time and generate a header file with credentials.
# .env is gitignored — each dev machine has its own copy created from .env.example.
# config.h uses #ifndef guards so env_generated.h definitions take precedence over defaults.
#
# We use a generated header instead of -D compile flags because CMake cannot pass
# preprocessor definitions containing '#' on the command line (it drops them silently).
set(ENV_FILE "${PROJECT_DIR}/.env")
set(ENV_GENERATED_H "${CMAKE_CURRENT_BINARY_DIR}/env_generated.h")

if(EXISTS "${ENV_FILE}")
    message(STATUS "Loading credentials from .env")
    file(STRINGS "${ENV_FILE}" ENV_LINES)

    set(ENV_HEADER_CONTENT "// Auto-generated from .env — do not edit\n#pragma once\n\n")
    foreach(LINE ${ENV_LINES})
        # Skip empty lines and comments
        if(LINE MATCHES "^[ \t]*#" OR LINE STREQUAL "")
            continue()
        endif()
        # Split on first '=' only
        string(REGEX MATCH "^([^=]+)=(.*)" _ "${LINE}")
        if(CMAKE_MATCH_1 AND DEFINED CMAKE_MATCH_2)
            set(ENV_KEY "${CMAKE_MATCH_1}")
            set(ENV_VAL "${CMAKE_MATCH_2}")
            # Strip leading/trailing whitespace from key
            string(STRIP "${ENV_KEY}" ENV_KEY)
            # Strip surrounding quotes from value if present
            string(REGEX REPLACE "^[\"']|[\"']$" "" ENV_VAL "${ENV_VAL}")
            string(APPEND ENV_HEADER_CONTENT "#define ${ENV_KEY} \"${ENV_VAL}\"\n")
        endif()
    endforeach()

    file(WRITE "${ENV_GENERATED_H}" "${ENV_HEADER_CONTENT}")
    message(STATUS "Generated ${ENV_GENERATED_H}")

    # Add the build directory to the include path so config.h can find env_generated.h
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
else()
    # Write an empty header so #include doesn't fail
    file(WRITE "${ENV_GENERATED_H}" "// No .env file found — using defaults from config.h\n#pragma once\n")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    message(WARNING "No .env file found at ${ENV_FILE} — using defaults from config.h. "
                    "Copy .env.example to .env and fill in your credentials.")
endif()
