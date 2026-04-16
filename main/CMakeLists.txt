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

# Parse .env file at build time and inject as compile definitions.
# .env is gitignored — each dev machine has its own copy created from .env.example.
# config.h uses #ifndef guards so these definitions take precedence over defaults.
set(ENV_FILE "${PROJECT_DIR}/.env")
if(EXISTS "${ENV_FILE}")
    message(STATUS "Loading credentials from .env")
    file(STRINGS "${ENV_FILE}" ENV_LINES)
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
            target_compile_definitions(${COMPONENT_LIB} PRIVATE ${ENV_KEY}="${ENV_VAL}")
        endif()
    endforeach()
else()
    message(WARNING "No .env file found at ${ENV_FILE} — using defaults from config.h. "
                    "Copy .env.example to .env and fill in your credentials.")
endif()
