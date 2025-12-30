#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_sntp.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"


#include "esp_wifi.h"
#include "mqtt_client.h"

#include "driver.h"


#include "credentials.h"
//see the README.md

static const char *TAG = "MQTT";

#define DEVICE_ID "faninv001"

#define MQTT_FULL_URI MQTT_SCHEME "://" MQTT_BROKER_URI

#define NOTIFY_SOURCE_MQTT_CONNECTED  BIT1


void init_time_sync(void)
{
    ESP_LOGI(TAG, "Initializing SNTP for Time Sync...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    
    while (timeinfo.tm_year < (2024 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    
    if (timeinfo.tm_year < (2024 - 1900)) {
        ESP_LOGE(TAG, "Failed to update system time. TLS might fail!");
    } else {
        ESP_LOGI(TAG, "Time set to: %s", asctime(&timeinfo));
    }
}








typedef void (*topic_handler_fn)(const char *data, int len);

// Structure to map a topic to a function
typedef struct {
    const char *topic;
    topic_handler_fn handler;
} mqtt_topic_map_t;





static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t mqtt_client;

TaskHandle_t mqtt_task_handle = NULL;

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
        spwm_start(DEFAULT_FREQ_HZ);
    } else {
        ESP_LOGI(TAG, "Inverter -> OFF");
        spwm_stop();
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

    spwm_set_target_frequency((int)freq);
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
            ESP_LOGI(TAG, "MQTT Connected to %s", MQTT_FULL_URI);
            ESP_LOGI(TAG, "Session present: %d", event->session_present);
            
            // Loop through the array and subscribe to everything
            for (int i = 0; i < LISTEN_TOPICS_COUNT; i++) {
                int msg_id = esp_mqtt_client_subscribe(client, listen_topics[i].topic, 1); // QoS 1
                if (msg_id == -1) {
                    ESP_LOGE(TAG, "Failed to subscribe to: %s", listen_topics[i].topic);
                } else {
                    ESP_LOGI(TAG, "Subscribing to: %s (Msg ID: %d)", listen_topics[i].topic, msg_id);
                }
            }
            if (mqtt_task_handle != NULL) {
                xTaskNotify(mqtt_task_handle, NOTIFY_SOURCE_MQTT_CONNECTED, eSetBits);
            }
            
            // Optional: Publish "Online" status (Retained)
            // esp_mqtt_client_publish(client, "home/inverter/" DEVICE_ID "/status", "online", 0, 1, 1);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            // Just debug info to confirm broker acked the subscription
            ESP_LOGD(TAG, "Subscription ACK received, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
             ESP_LOGD(TAG, "Unsubscription ACK received, msg_id=%d", event->msg_id);
             break;

        case MQTT_EVENT_PUBLISHED:
             ESP_LOGD(TAG, "Publish ACK received, msg_id=%d", event->msg_id);
             break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Data on %.*s", event->topic_len, event->topic);
            
            // Generic Dispatcher: Find the matching topic in our array
            bool handled = false;
            for (int i = 0; i < LISTEN_TOPICS_COUNT; i++) {
                // Ensure length matches and strings match
                if (strlen(listen_topics[i].topic) == event->topic_len &&
                    strncmp(event->topic, listen_topics[i].topic, event->topic_len) == 0) {
                    
                    if(!listen_topics[i].handler)
                    {
                        ESP_LOGE(TAG, "Topic match found but Handler is NULL!");
                        break;
                    }

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
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected. Waiting for auto-reconnect...");
            break;

        
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Transport Error! (Check IP/Port/Wifi)");
                ESP_LOGE(TAG, "Last errno: 0x%x", event->error_handle->esp_transport_sock_errno);
                ESP_LOGE(TAG, "TLS/SSL Stack Error: 0x%x", event->error_handle->esp_tls_stack_err);
                ESP_LOGE(TAG, "Last ESP-TLS Error: 0x%x", event->error_handle->esp_tls_last_esp_err);
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                 ESP_LOGE(TAG, "Connection Refused! (Check Username/Password/ClientID)");
            } else {
                 ESP_LOGE(TAG, "Unknown Error Type: %d", event->error_handle->error_type);
            }
            break;
        default:
            ESP_LOGD(TAG, "Other event id:%d", event->event_id);
            break;
    }
}



void mqtt_publish_task(void *pvParameters)
{
    // Register self
    mqtt_task_handle = xTaskGetCurrentTaskHandle();
    spwm_register_mqtt(mqtt_task_handle);

    char payload[32];
    spwm_runtime_state_t current_state; 
    
    // "Last Known" state (Initialize to impossible values to force first update)
    spwm_runtime_state_t last_state = { .current_frequency = 0, .mod_index =0., .target_frequency = 0, .running = false };

    uint32_t notification_value = 0; //ignored for now

    while (1) {
        // 1. Wait for "Give" signal (Block forever)
        xTaskNotifyWait(0x00, 0xFFFFFFFF, &notification_value, portMAX_DELAY);

        bool force_update = false;

        if (notification_value & NOTIFY_SOURCE_MQTT_CONNECTED) {
            ESP_LOGI(TAG, "MQTT Ready: Forcing full state refresh");
            force_update = true;
        }

        // 2. Snapshot current system state
        spwm_get_state(&current_state);

        // 3. Diff & Publish - FREQUENCY
        if (current_state.current_frequency != last_state.current_frequency || force_update) {
            snprintf(payload, sizeof(payload), "%d", current_state.current_frequency);
            esp_mqtt_client_publish(mqtt_client, 
                                    "home/inverter/" DEVICE_ID "/status/frequency", 
                                    payload, 0, 1, 0);
            
            last_state.current_frequency = current_state.current_frequency; // Update last known
            ESP_LOGI(TAG, "MQTT: Freq updated to %d", last_state.current_frequency);
        }

        // 4. Diff & Publish - STATUS (ON/OFF)
        if (current_state.running != last_state.running || force_update) {
            const char* state_str = current_state.running ? "ON" : "OFF";
            esp_mqtt_client_publish(mqtt_client, 
                                    "home/inverter/" DEVICE_ID "/status/state", 
                                    state_str, 0, 1, 1);
            
            last_state.running = current_state.running; // Update last known
            ESP_LOGI(TAG, "MQTT: State updated to %s", state_str);
        }

        // Throttle updates slightly to prevent WiFi congestion during fast ramping
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}


void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_FULL_URI,
        .broker.address.port = MQTT_PORT,

        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,

        .broker.verification.certificate = CACERTPEM,
        //#ifdef MQTT_USE_TLS
        //.broker.verification.certificate_len = strlen(CACERTPEM),
        //#endif
        .broker.verification.skip_cert_common_name_check = true,
        
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    /* The modern way to register events in ESP-IDF */
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    xTaskCreate(mqtt_publish_task, "mqtt_pub_task", 4096, NULL, 5, NULL);
}