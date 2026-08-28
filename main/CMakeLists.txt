idf_component_register(SRCS "main.cc"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES
                           app_framework
                           startup_ui
                           display_service
                           network_service
                           storage_service
                           ota_service)