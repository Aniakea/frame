idf_component_register(REQUIRES littlefs)

littlefs_create_partition_image(assets_fs "../../assets" FLASH_IN_PROJECT)
