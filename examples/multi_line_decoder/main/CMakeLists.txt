idf_component_register(SRCS "main.c" "signal_generator.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES pulse-decoder esp_driver_gpio)
