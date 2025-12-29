#ifndef DRIVER_H
#define DRIVER_H



#define MIN_FREQ_HZ             20
#define MAX_FREQ_HZ             60
#define DEFAULT_FREQ_HZ         50
#define NOMINAL_FREQ_HZ         ((float)DEFAULT_FREQ_HZ)
#define MIN_VOLTAGE_BOOST        0.15f // 15% minimum voltage to prevent stalling




void setup_mcpwm();

void spwm_start(int frequency);
void spwm_stop(void);
void spwm_set_target_frequency(int frequency);


#endif