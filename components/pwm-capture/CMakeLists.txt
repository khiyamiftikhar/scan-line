idf_component_register(SRCS "pwm_capture.c"
                        INCLUDE_DIRS "." "../capture-line-interface" "../event-data-include"
                        REQUIRES  esp_driver_mcpwm )