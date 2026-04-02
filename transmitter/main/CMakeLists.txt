idf_component_register(SRCS "hid_host_example.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_rmt esp_driver_gpio driver)
