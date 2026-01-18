/*
 * ESP32 SPWM Generation (ESP-IDF v5.x)
 * Status: ROBUST / DYNAMIC CLOCK ADAPTATION
 */

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_clk_tree.h"

#include "driver.h"


typedef struct {
    int timer_id;
    void *group;
    void *fsm;
    void *spinlock;
    uint32_t placeholder1;
    void *intr;
    uint32_t resolution_hz; 
    uint32_t peak_ticks;    // <--- THIS IS THE TRUTH
} mcpwm_timer_impl_t;


// ----------------------------------------------------------------------------------
// CONFIGURATION
// ----------------------------------------------------------------------------------
#define SPWM_LEG1_LOW_PIN       12
#define SPWM_LEG1_HIGH_PIN      13
#define SPWM_LEG2_LOW_PIN       14
#define SPWM_LEG2_HIGH_PIN      27

#define CARRIER_FREQ_HZ         20000UL   // 20kHz
#define DEAD_TIME_NS            800UL     // 500ns Deadtime



#define MAX_SAMPLES             (CARRIER_FREQ_HZ / MIN_FREQ_HZ)
#define MOD_INDEX               1U
#define MAX_DUTY_CYCLE_PERC     0.95f //duty cycle maximum; caps require re-charging

static const char *TAG = "SPWM";

#define TIMER_RESOLUTION_HZ 10000000UL
#define PEAK_TICKS (TIMER_RESOLUTION_HZ / (CARRIER_FREQ_HZ * 2))
#define MAX_TICKS ((uint32_t)(PEAK_TICKS*0.95f))




static DRAM_ATTR uint32_t sine_lut[2][MAX_SAMPLES];
static volatile uint32_t * volatile active_lut = sine_lut[0];
static volatile uint32_t * volatile pending_lut = sine_lut[1];

static SemaphoreHandle_t lut_calc_mutex = NULL;



typedef struct {
    volatile bool enabled;
    volatile int current_freq;
    volatile float mod_index;
    volatile int samples;
} spwm_internal_state_t;

static volatile spwm_internal_state_t active_state = {
  .enabled = false,  //tied to ISR update, requested from thread
  .current_freq = 0, //tied to LUT update, requested from thread
  .mod_index = 0.f, //tied to LUT update, requested from thread
  .samples = 0
};

static volatile spwm_internal_state_t pending_state = {
  .enabled = false,  //tied to ISR update, requested from thread
  .current_freq = 0, //tied to LUT update, requested from thread
  .mod_index = 0.f, //tied to LUT update, requested from thread
  .samples = 0
};



static volatile int target_freq = 0;
static volatile bool g_update_pending = false;  

static volatile int g_current_sample_idx = 0;

static EventGroupHandle_t mqtt_dirty_flags = NULL;
static TaskHandle_t mqtt_task_handle = NULL;


mcpwm_cmpr_handle_t comparator_leg1 = NULL;
mcpwm_cmpr_handle_t comparator_leg2 = NULL;

mcpwm_gen_handle_t gen_leg1_h = NULL;
mcpwm_gen_handle_t gen_leg1_l = NULL;
mcpwm_gen_handle_t gen_leg2_h = NULL;
mcpwm_gen_handle_t gen_leg2_l = NULL;

mcpwm_timer_handle_t timer = NULL;



static inline __attribute__((always_inline)) void swap_lut_pointers(volatile uint32_t * volatile *a, volatile uint32_t * volatile *b)
{
    volatile uint32_t *temp = *a;
    *a = *b;
    *b = temp;
}


#define DEFAULT_FREQ_STEP 2 //for smooth changes in the frequency



static portMUX_TYPE spwm_lock = portMUX_INITIALIZER_UNLOCKED;
//metadata about the SPWM module



void spwm_get_state(spwm_runtime_state_t *out)
{
    taskENTER_CRITICAL(&spwm_lock);
    out->running              = active_state.enabled;
    out->current_frequency    = active_state.current_freq;
    out->target_frequency     = target_freq;
    out->mod_index            = active_state.mod_index;
    out->fuzzy_en             = false;
    out->silent               = false;
    out->update_pending       = g_update_pending;
    taskEXIT_CRITICAL(&spwm_lock);
}




static void freq_update_task(void *);

// ----------------------------------------------------------------------------------
// MATH (Task Context)
// ----------------------------------------------------------------------------------

void set_new_frequency(int new_freq)
{
    xSemaphoreTake(lut_calc_mutex, portMAX_DELAY);

    float freq_hz = new_freq;


    if (freq_hz < MIN_FREQ_HZ) freq_hz = MIN_FREQ_HZ;
    if (freq_hz > MAX_FREQ_HZ) freq_hz = MAX_FREQ_HZ;

    float v_f_ratio = freq_hz / NOMINAL_FREQ_HZ;
    // Ensure we don't drop below a minimum torque threshold (Boost)
    if (v_f_ratio < MIN_VOLTAGE_BOOST) v_f_ratio = MIN_VOLTAGE_BOOST;
    // Ensure we never exceed 100% duty cycle
    if (v_f_ratio > 1.0f) v_f_ratio = 1.0f;

    


    int samples = (int)(CARRIER_FREQ_HZ / freq_hz);
    if (samples > MAX_SAMPLES) samples = MAX_SAMPLES;
    
    uint32_t *target_buffer = (uint32_t *)pending_lut; 
    

    for (int i = 0; i < samples; i++) {
        float angle = (2.0f * M_PI * i) / samples;
        float sin_val = fabsf(sin(angle));
        
        // Calculate
        uint32_t duty_ticks = (uint32_t)(PEAK_TICKS * sin_val * v_f_ratio);
        
        // PRE-CALCULATION CLAMP
        if (duty_ticks > MAX_TICKS) {
            duty_ticks = MAX_TICKS;
        }
        
        target_buffer[i] = duty_ticks;
    }
    ESP_LOGI(TAG, "Freq Req: %.2f Hz | Samples: %d | Time per Sample: %.2f us", 
             freq_hz, 
             samples, 
             (1000000.0 / CARRIER_FREQ_HZ));

    taskENTER_CRITICAL(&spwm_lock);

    bool changed = false;

    if(pending_state.mod_index != v_f_ratio || pending_state.current_freq != new_freq) changed = true;

    if(pending_state.mod_index != v_f_ratio) 
        xEventGroupSetBits(mqtt_dirty_flags, MQTT_UPDATE_MOD_INDEX_BIT); //may not change, when freq does
    pending_state.mod_index = v_f_ratio;
    if(pending_state.current_freq != new_freq) 
        xEventGroupSetBits(mqtt_dirty_flags, MQTT_UPDATE_FREQ_BIT); //not fully bulletproof, but reduces traffic
    pending_state.current_freq = new_freq;


    pending_state.samples = samples;
    g_update_pending = true; 
    taskEXIT_CRITICAL(&spwm_lock);
    xSemaphoreGive(lut_calc_mutex);

}


void force_new_frequency(void)
{
    swap_lut_pointers(&active_lut, &pending_lut);
    //g_samples_per_cycle = pending_state.samples;
    active_state = pending_state;
    g_update_pending = false;    
}



// ----------------------------------------------------------------------------------
// ISR (High Speed)
// ----------------------------------------------------------------------------------
static bool IRAM_ATTR mcpwm_timer_event_cb(mcpwm_timer_handle_t timer, const mcpwm_timer_event_data_t *edata, void *user_ctx)
{
    if (active_state.samples == 0) return false;

    // 1. Cycle End Check & LUT Swap
    if (g_current_sample_idx >= active_state.samples) {
        g_current_sample_idx = 0;


        if (g_update_pending) {

            swap_lut_pointers(&active_lut, &pending_lut);
            //g_samples_per_cycle = g_pending_samples;
            active_state = pending_state;

            if (mqtt_task_handle != NULL) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                
                // eSetBits: Works exactly like EventGroup (OR logic)
                xTaskNotifyFromISR(mqtt_task_handle, 
                                   NOTIFY_SOURCE_DRIVER, 
                                   eSetBits, 
                                   &xHigherPriorityTaskWoken);
                
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
            }

            g_update_pending = false;
        }
        
    }

    if(active_state.enabled == false)
    {
        mcpwm_comparator_set_compare_value(comparator_leg1, 0);
        mcpwm_comparator_set_compare_value(comparator_leg2, 0);
        return false; 
    }

    // 2. Update HF SPWM (Leg 1)
    uint32_t cmp_val = active_lut[g_current_sample_idx];
    if (cmp_val > MAX_TICKS) cmp_val = MAX_TICKS; // Safety Clamp
    mcpwm_comparator_set_compare_value(comparator_leg1, cmp_val);


    // 3. Update LF Commutation (Leg 2) via Hardware Force
    // We only issue the command twice per cycle to save overhead
    int half_cycle = active_state.samples / 2;

    if (g_current_sample_idx == 0) {
        // First Half: Leg 2 High=ON, Leg 2 Low=OFF
        // (Force Level handles overrides; Deadtime module handles safety)
        mcpwm_comparator_set_compare_value(comparator_leg2, PEAK_TICKS );  // Hold High; unsafe, critical fix required
    } 
    else if (g_current_sample_idx == half_cycle) {
        // Second Half: Leg 2 High=OFF, Leg 2 Low=ON
        mcpwm_comparator_set_compare_value(comparator_leg2, 0); // Hold Low
    }

    g_current_sample_idx++;
    return false;
}



//do not touch, confirmed to work
void setup_mcpwm()
{
    int pins[] = {
        SPWM_LEG1_LOW_PIN,
        SPWM_LEG1_HIGH_PIN,
        SPWM_LEG2_LOW_PIN,
        SPWM_LEG2_HIGH_PIN
    };

    for (int i = 0; i < 4; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_DISABLE);
    
    }

    lut_calc_mutex = xSemaphoreCreateMutex();
    mqtt_dirty_flags = xEventGroupCreate();
    

    // -------------------------------------------------------
    // 1. Timer Setup (Shared by both legs)
    // -------------------------------------------------------
    
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = TIMER_RESOLUTION_HZ,
        .period_ticks = PEAK_TICKS * 2, 
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP_DOWN,
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &timer));

    // -------------------------------------------------------
    // 2. Operator Setup
    // -------------------------------------------------------
    mcpwm_oper_handle_t oper_leg1 = NULL;
    mcpwm_oper_handle_t oper_leg2 = NULL;
    
    mcpwm_operator_config_t operator_config = { .group_id = 0 };

    // Operator for Leg 1 (HF SPWM)
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &oper_leg1));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper_leg1, timer));

    // Operator for Leg 2 (Fundamental Square)
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &oper_leg2));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper_leg2, timer));

    // -------------------------------------------------------
    // 3. Comparator Setup (Leg 1 only)
    // -------------------------------------------------------
    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true, 
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(oper_leg1, &comparator_config, &comparator_leg1));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(comparator_leg1, 0));

    // Leg 2 Comparator (NEW: Required for safe ISR control)
    ESP_ERROR_CHECK(mcpwm_new_comparator(oper_leg2, &comparator_config, &comparator_leg2));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(comparator_leg2, 0));

    // -------------------------------------------------------
    // 4. Generator Setup
    // -------------------------------------------------------

    
    mcpwm_generator_config_t gen_config = {};

    // -- LEG 1 Generators (SPWM) --
    gen_config.gen_gpio_num = SPWM_LEG1_HIGH_PIN;
    ESP_ERROR_CHECK(mcpwm_new_generator(oper_leg1, &gen_config, &gen_leg1_h));
    gen_config.gen_gpio_num = SPWM_LEG1_LOW_PIN;
    ESP_ERROR_CHECK(mcpwm_new_generator(oper_leg1, &gen_config, &gen_leg1_l));

    // -- LEG 2 Generators (Fundamental) --
    gen_config.gen_gpio_num = SPWM_LEG2_HIGH_PIN;
    ESP_ERROR_CHECK(mcpwm_new_generator(oper_leg2, &gen_config, &gen_leg2_h));
    gen_config.gen_gpio_num = SPWM_LEG2_LOW_PIN;
    ESP_ERROR_CHECK(mcpwm_new_generator(oper_leg2, &gen_config, &gen_leg2_l));

    // -------------------------------------------------------
    // 5. Generator Actions (Leg 1 Only)
    // Leg 2 actions are controlled via Force Level in ISR
    // -------------------------------------------------------
    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(
        gen_leg1_h, 
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, comparator_leg1, MCPWM_GEN_ACTION_LOW),
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_DOWN, comparator_leg1, MCPWM_GEN_ACTION_HIGH),
        MCPWM_GEN_COMPARE_EVENT_ACTION_END()
    ));
    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_timer_event(gen_leg1_h,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH),
        MCPWM_GEN_TIMER_EVENT_ACTION_END()
    ));

    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_compare_event(gen_leg2_h, 
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, comparator_leg2, MCPWM_GEN_ACTION_LOW),
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_DOWN, comparator_leg2, MCPWM_GEN_ACTION_HIGH),
        MCPWM_GEN_COMPARE_EVENT_ACTION_END()));
    ESP_ERROR_CHECK(mcpwm_generator_set_actions_on_timer_event(gen_leg2_h,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH),
        MCPWM_GEN_TIMER_EVENT_ACTION_END()));


    

    // -------------------------------------------------------
    // 6. Dead Time Setup (Applied to both legs)
    // -------------------------------------------------------
    uint32_t dt_ticks = (uint32_t)( (uint64_t)(DEAD_TIME_NS * TIMER_RESOLUTION_HZ) / 1000000000UL);
    
    // -- LEG 1 DEAD TIME --
    // High side: standard delay
    mcpwm_dead_time_config_t dt_config_h = { .posedge_delay_ticks = dt_ticks };
    ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(gen_leg1_h, gen_leg1_h, &dt_config_h));
    
    // Low side: Takes Gen1_H as input, Inverts it, Apply delay
    mcpwm_dead_time_config_t dt_config_l = { 
        .negedge_delay_ticks = dt_ticks,
        .flags.invert_output = true 
    };
    ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(gen_leg1_h, gen_leg1_l, &dt_config_l)); 

    ESP_LOGI(TAG, "Dead time: %lu", dt_ticks );

    // -- LEG 2 DEAD TIME --
    // Even though Leg 2 switches at 50Hz, Dead Time is required for the transition.
    // High side: Self-input
    ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(gen_leg2_h, gen_leg2_h, &dt_config_h));

    // Low side: Takes Gen2_H as input, Inverts it.
    // This allows us to only force Gen2_H in the ISR, and Gen2_L follows automatically (inverted).
    ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(gen_leg2_h, gen_leg2_l, &dt_config_l));

    // 7. Start
    mcpwm_timer_event_callbacks_t cbs = { .on_empty = mcpwm_timer_event_cb };
    ESP_ERROR_CHECK(mcpwm_timer_register_event_callbacks(timer, &cbs, NULL));

    ESP_ERROR_CHECK(mcpwm_timer_enable(timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));

    mcpwm_generator_set_force_level(gen_leg1_h, 0, true);
    mcpwm_generator_set_force_level(gen_leg1_l, 0, true);
    mcpwm_generator_set_force_level(gen_leg2_h, 0, true);
    mcpwm_generator_set_force_level(gen_leg2_l, 0, true);
    
    xTaskCreatePinnedToCore(freq_update_task, "freq_task", 4096, NULL, 5, NULL, 1);
}


void spwm_register_mqtt(TaskHandle_t handle)
{
    mqtt_task_handle = handle;
}


//think about snapshoting the active & pending states before logic operations
void spwm_start(int frequency)
{

    
    // 1. Check if we are fully stopped
    if (!active_state.enabled) {
        // Standard Cold Start logic
        ESP_LOGI(TAG, "Inverter STARTING.");
        

        set_new_frequency(50); // Calc 50Hz LUT

        taskENTER_CRITICAL(&spwm_lock);
        // Reset to safe defaults
        
        pending_state.enabled = true;
        
        // Safe Initial Swap (since hardware is off, no glitches possible)
        swap_lut_pointers(&active_lut, &pending_lut);
        
        active_state = pending_state;
        
        // Un-force the pins (The ISR is likely running but doing nothing)
        mcpwm_generator_set_force_level(gen_leg1_h, -1, true); 
        mcpwm_generator_set_force_level(gen_leg1_l, -1, true);
        mcpwm_generator_set_force_level(gen_leg2_h, -1, true);
        mcpwm_generator_set_force_level(gen_leg2_l, -1, true);

        g_update_pending = false; 

        taskEXIT_CRITICAL(&spwm_lock);

        spwm_set_target_frequency(frequency);

        if (mqtt_task_handle) {
            xTaskNotify(mqtt_task_handle, NOTIFY_SOURCE_DRIVER, eSetBits);
        }
        
        return;
    }

    // 2. Check if we are in the "Stopping" state (The Zombie State)
    // active is TRUE, but pending is FALSE.
    if (g_update_pending && !pending_state.enabled) {
        ESP_LOGI(TAG, "Inverter Stop ABORTED. Resuming operation.");
        
        set_new_frequency(frequency);
        taskENTER_CRITICAL(&spwm_lock);
        pending_state.enabled = true; // Cancel the stop
        
        // Optionally apply new frequency immediately if requested
        
        g_update_pending = true; // Ensure ISR picks up the "True" enabled state
        taskEXIT_CRITICAL(&spwm_lock);
        
        target_freq = frequency;
        return;
    }

    // 3. Otherwise, we are just running normally
    ESP_LOGW(TAG, "Inverter start requested while already running.");
}




void spwm_stop(void)
{
    target_freq = 0;
    taskENTER_CRITICAL(&spwm_lock);
    pending_state.enabled = false;
    pending_state.current_freq = 0;
    pending_state.mod_index = 0.0f;
    g_update_pending = true;
    taskEXIT_CRITICAL(&spwm_lock);
    ESP_LOGW(TAG, "Inverter STOP requested (Will halt at next zero-cross)");

    if (mqtt_dirty_flags) 
    {
        xEventGroupSetBits(mqtt_dirty_flags, MQTT_UPDATE_STATUS_BIT);
        xEventGroupSetBits(mqtt_dirty_flags, MQTT_UPDATE_FREQ_BIT);
        xEventGroupSetBits(mqtt_dirty_flags, MQTT_UPDATE_MOD_INDEX_BIT);
    }
    return;
}




void spwm_set_target_frequency(int frequency)
{
    if(!active_state.enabled)
    {
        ESP_LOGW(TAG, "Inverter FREQ_CHNG requested while not running. Starting.");
        spwm_start(frequency);
        return;
    }

    frequency = frequency < MAX_FREQ_HZ ? frequency : MAX_FREQ_HZ;
    frequency = frequency > MIN_FREQ_HZ ? frequency : MIN_FREQ_HZ;

    if(target_freq != frequency) xEventGroupSetBits(mqtt_dirty_flags,MQTT_UPDATE_TARGT_BIT);
    target_freq = frequency;    
}



static void freq_update_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(500)); 
    while (1) {
        if(
            active_state.enabled && 
            !g_update_pending && //no update requests so far, we are the only ones staging an update
//            pending_state.current_freq > 0 && 
            target_freq != active_state.current_freq)
        {
            int diff = (target_freq  - active_state.current_freq );

            int calc_freq = DEFAULT_FREQ_HZ;

            if( abs(diff) < DEFAULT_FREQ_STEP)
            {
                calc_freq = target_freq;
            }
            else
            {
                int sign = diff > 0 ? 1 : -1;
                calc_freq = active_state.current_freq + sign*DEFAULT_FREQ_STEP;
            }

            set_new_frequency(calc_freq);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}