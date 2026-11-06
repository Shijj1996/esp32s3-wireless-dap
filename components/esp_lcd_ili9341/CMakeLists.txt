idf_component_register(SRCS "esp_lcd_ili9341.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "driver" "esp_lcd")

# 原本由 cmake_utilities 的 cu_pkg_define_version() 注入，这里手动给
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_ILI9341_VER_MAJOR=1
    ESP_LCD_ILI9341_VER_MINOR=2
    ESP_LCD_ILI9341_VER_PATCH=0)