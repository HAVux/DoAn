#ifndef WIFI_H
#define WIFI_H
#include "attendance_device.h"
#include <stdint.h>

#define EXAMPLE_ESP_WIFI_SSID      "Attendance_Device"
#define EXAMPLE_ESP_WIFI_PASS      "123456788"
#define EXAMPLE_ESP_WIFI_CHANNEL   1
#define EXAMPLE_MAX_STA_CONN       4
#define EXAMPLE_ESP_MAXIMUM_RETRY  5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

typedef struct {
    char    *username;
    char    *password;
} basic_auth_info_t;

void Start_connection(Attendance_Device_t *device);
void send_post_login_request(Attendance_Device_t *device);
void send_create_invoice_request(Attendance_Device_t *device, int quantity[]);
void send_invoice_request(Attendance_Device_t *device);
void send_create_qr_request(Attendance_Device_t *device);
void send_check_invoice_request(Attendance_Device_t *device);
void send_cancel_invoice_request(Attendance_Device_t *device);
#endif // WIFI_H