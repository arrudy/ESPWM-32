#ifndef DRIVER_H
#define DRIVER_H



#define MIN_FREQ_HZ             20
#define MAX_FREQ_HZ             60
#define DEFAULT_FREQ_HZ         50
#define NOMINAL_FREQ_HZ         ((float)DEFAULT_FREQ_HZ)
#define MIN_VOLTAGE_BOOST        0.15f // 15% minimum voltage to prevent stalling



void set_new_frequency(int freq_hz);
void force_new_frequency(void);
void setup_mcpwm();

void request_new_frequency(int new_target);

//void freq_update_task(void *pvParameters);

#endif