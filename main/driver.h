#ifndef DRIVER_H
#define DRIVER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define MIN_FREQ_HZ             30
#define MAX_FREQ_HZ             60
#define DEFAULT_FREQ_HZ         50
#define NOMINAL_FREQ_HZ         ((float)DEFAULT_FREQ_HZ)
#define MIN_VOLTAGE_BOOST        0.15f // 15% minimum voltage to prevent stalling


#define NOTIFY_SOURCE_DRIVER    BIT0 //MQTT transmit task is notified with this bit set to 1

#define MQTT_UPDATE_STATUS_BIT      BIT0  // e.g. "running", "stopped"
#define MQTT_UPDATE_FREQ_BIT        BIT1  // e.g. 50, 60
#define MQTT_UPDATE_TARGT_BIT       BIT2
#define MQTT_UPDATE_MOD_INDEX_BIT   BIT3
#define MQTT_UPDATE_DIFFS_STEP_BIT  BIT4  // not needed for now?


void setup_mcpwm();

void spwm_start(int frequency);
void spwm_stop(void);
void spwm_set_target_frequency(int frequency);


/**
 * @brief MQTT-related API
 * Wait for notification, then probe the state.
 */

typedef struct
{
    bool running;
    int current_frequency;
    int target_frequency;
    float mod_index;
    bool fuzzy_en;
    bool silent;
    bool update_pending;
} spwm_runtime_state_t;

void spwm_register_mqtt(TaskHandle_t handle);
void spwm_get_state(spwm_runtime_state_t *out);


#endif