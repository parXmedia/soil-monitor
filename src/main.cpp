#if defined(DEVICE_ROLE_DISPLAY) && defined(DEVICE_ROLE_SENSOR)
#error "Choose exactly one firmware role"
#elif !defined(DEVICE_ROLE_DISPLAY) && !defined(DEVICE_ROLE_SENSOR)
#error "Choose display_receiver or sensor_transmitter environment"
#endif
