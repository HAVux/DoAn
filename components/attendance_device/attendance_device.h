// attendance_device.h

#ifndef ATTENDANCE_DEVICE_H
#define ATTENDANCE_DEVICE_H

#include <stdbool.h>
#include "product.h"

typedef struct {
    char *ssid;
    char *pass;
    char *Client_id;
    char *Client_secret;
    char *Business_name;
    int number_of_products;
    char *adminPass;
    char *supporterPass;
    Product products[100];
    bool connected;
    char *token;
    char *invoice_id;
    char* image;
    bool invoice_status;
} Attendance_Device_t;

#endif // ATTENDANCE_DEVICE_H