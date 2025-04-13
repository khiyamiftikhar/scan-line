idf_component_register(SRCS "scan_manager.c"
                        INCLUDE_DIRS "." "../event-data-include"
                        PRIV_REQUIRES "pwm-capture")