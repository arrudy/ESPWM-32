#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "mqtt_client.h"

#include "driver.h"


#include "credentials.h"
/*
required constants:

#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASS      "YOUR_WIFI_PASSWORD"

#define MQTT_BROKER_URI "mqtt://your.address.here:port"
*/

static const char *TAG = "MQTT";

#define DEVICE_ID "inv001"


typedef void (*topic_handler_fn)(const char *data, int len);

// Structure to map a topic to a function
typedef struct {
    const char *topic;
    topic_handler_fn handler;
} mqtt_topic_map_t;





static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t mqtt_client;

/* ===================== WIFI ===================== */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected");
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            //.cert_pem = (const char *)server_root_cert_pem, //for TLS
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi init done");
}

/* ===================== MQTT ===================== */







void handle_state(const char* data, int len) {

    //placeholder
    if (strncmp(data, "ON", len) == 0) {
        ESP_LOGI(TAG, "Inverter -> ON");
        request_new_frequency(DEFAULT_FREQ_HZ);
    } else {
        ESP_LOGI(TAG, "Inverter -> OFF");
        request_new_frequency(0);
    }
}


void handle_frequency(const char* data, int len) {
    char buf[16] = {0};
    int copy_len = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, data, copy_len);

    char *endptr;
    long freq = strtol(buf, &endptr, 10);

    // Check if conversion actually happened
    if (endptr == buf) {
        ESP_LOGE(TAG, "Invalid integer received: %s", buf);
        return;
    }

    ESP_LOGI(TAG, "Frequency request of %ld Hz", freq);

    request_new_frequency((int)freq);
}



static const mqtt_topic_map_t listen_topics[] = {
    { "home/inverter/" DEVICE_ID "/control/state", handle_state },
    { "home/inverter/" DEVICE_ID "/control/frequency",  handle_frequency }
};


#define LISTEN_TOPICS_COUNT (sizeof(listen_topics) / sizeof(listen_topics[0]))



static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected. Subscribing to topics...");
            // Loop through the array and subscribe to everything
            for (int i = 0; i < LISTEN_TOPICS_COUNT; i++) {
                esp_mqtt_client_subscribe(client, listen_topics[i].topic, 1);
                ESP_LOGI(TAG, "Subscribed to: %s", listen_topics[i].topic);
            }
            //esp_mqtt_client_publish(client, TOPIC_STATUS, "online", 0, 1, 0);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Data on %.*s", event->topic_len, event->topic);
            
            // Generic Dispatcher: Find the matching topic in our array
            bool handled = false;
            for (int i = 0; i < LISTEN_TOPICS_COUNT; i++) {
                // Ensure length matches and strings match
                if (strlen(listen_topics[i].topic) == event->topic_len &&
                    strncmp(event->topic, listen_topics[i].topic, event->topic_len) == 0) {
                    
                    // Execute the associated handler
                    listen_topics[i].handler(event->data, event->data_len);
                    handled = true;
                    break; 
                }
            }
            
            if (!handled) {
                ESP_LOGW(TAG, "No handler found for topic: %.*s", event->topic_len, event->topic);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error");
            break;
        default:
            break;
    }
}


void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    /* The modern way to register events in ESP-IDF */
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}