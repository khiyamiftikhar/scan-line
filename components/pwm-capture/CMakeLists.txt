idf_component_register(SRCS "pwm_capture.c"
                        INCLUDE_DIRS "." "../capture-line-interface" "../common"
                        REQUIRES  esp_driver_mcpwm )