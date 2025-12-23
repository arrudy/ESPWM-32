#include "driver.h"
#include "mqtt.h"


#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_clk_tree.h"
#include "nvs_flash.h"


static const char *TAG = "MAIN";


void app_main(void)
{
    set_new_frequency(DEFAULT_FREQ_HZ);
    force_new_frequency();
    setup_mcpwm();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    mqtt_init();

    for(;;){vTaskDelay(1000);}

    
}