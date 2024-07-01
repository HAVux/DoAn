#include "wifi.h"
#include <stdio.h>
#include <esp_spiffs.h>
#include "esp_log.h"
#include "string.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include <stdlib.h>
#include "mbedtls/base64.h"
#include "esp_crt_bundle.h"

#include "nvs_flash.h"
#include <ctype.h>

static const char *TAG = "WIFI";
static char *response_buffer = NULL;
static int s_retry_num = 0;
static int total_read_len = 0; 
static EventGroupHandle_t s_wifi_event_group;
#define MIN(a,b) (((a)<(b))?(a):(b))
#define TOKEN_LENGTH 20  // Define the length of the token
#define MAX_HTTP_RECV_BUFFER 2048 // Adjust size as needed
char expected_auth_admin_header[256];
char expected_auth_supporter_header[256];

Attendance_Device_t* device_to_use;

void Get_Token(Attendance_Device_t *device){
    ESP_LOGI(TAG,"Response: %s",response_buffer);
    char *token_str = "\"access_token\":\"";
    char *token_ptr = strstr(response_buffer, token_str);
    if (token_ptr) {
        char *substring = token_ptr + strlen(token_str); // Move past "access_token":"
        char *end_ptr = strstr(substring, "\""); // Find the end of the token
        if (end_ptr) {
            int token_length = end_ptr - substring;
            device->token = malloc(token_length + 1); // +1 for the null-terminator
            strncpy(device->token, substring, token_length);
            device->token[token_length] = '\0'; // Null-terminate the string
            ESP_LOGI(TAG, "Token: %s", device->token);
        } else {
            ESP_LOGE(TAG, "Token end not found");
        }
    } else {
        ESP_LOGE(TAG, "Token not found in response");
    }
}

void get_invoice_id(Attendance_Device_t *device){
    ESP_LOGI(TAG,"Response: %s",response_buffer);
    char *invoice_str = "\"id\":\"";
    char *invoice_ptr = strstr(response_buffer, invoice_str);
    if (invoice_ptr) {
        char *substring = invoice_ptr + strlen(invoice_str); // Move past "id":"
        char *end_ptr = strstr(substring, "\""); // Find the end of the token
        if (end_ptr) {
            int invoice_length = end_ptr - substring;
            device->invoice_id = malloc(invoice_length + 1); // +1 for the null-terminator
            strncpy(device->invoice_id, substring, invoice_length);
            device->invoice_id[invoice_length] = '\0'; // Null-terminate the string
            device->invoice_status = false;
            ESP_LOGI(TAG, "Invoice ID: %s", device->invoice_id);
        } else {
            ESP_LOGE(TAG, "Invoice ID end not found");
        }
    } else {
        ESP_LOGE(TAG, "Invoice ID not found in response");
    }
}

void Get_Image(Attendance_Device_t *device){
    ESP_LOGI(TAG,"Response: %s",response_buffer);
    char *image_str = "\"image\":\"";
    char *image_ptr = strstr(response_buffer, image_str);
    if (image_ptr) {
        char *substring = image_ptr + strlen(image_str); // Move past "image":"
        char *end_ptr = strstr(substring, "\""); // Find the end of the image value
        if (end_ptr) {
            int image_length = end_ptr - substring;
            device->image = malloc(image_length + 1); // +1 for the null-terminator
            strncpy(device->image, substring, image_length);
            device->image[image_length] = '\0'; // Null-terminate the string
            ESP_LOGI(TAG, "Image: %s", device->image);
        } else {
            ESP_LOGE(TAG, "Image end not found");
        }
    } else {
        ESP_LOGE(TAG, "Image not found in response");
    }
}
void get_invoice_status(Attendance_Device_t *device){
    ESP_LOGI(TAG,"Response: %s",response_buffer);
    char *status_str = "\"status\":\"";
    char *status_ptr = strstr(response_buffer, status_str);
    if (status_ptr) {
        char *substring = status_ptr + strlen(status_str); // Move past "status":"
        char *end_ptr = strstr(substring, "\""); // Find the end of the status value
        if (end_ptr) {
            int status_length = end_ptr - substring;
            char *status = malloc(status_length + 1); // +1 for the null-terminator
            strncpy(status, substring, status_length);
            status[status_length] = '\0'; // Null-terminate the string
            ESP_LOGI(TAG, "Status: %s", status);
            if (strcmp(status, "PAID") == 0) {
                device->invoice_status = true;
                device->invoice_id = NULL;
                device->image = NULL;
            } else {
                device->invoice_status = false;
            }
            free(status);
        } else {
            ESP_LOGE(TAG, "Status end not found");
        }
    } else {
        ESP_LOGE(TAG, "Status not found in response");
    }
}
esp_err_t client_event_post_handler(esp_http_client_event_handle_t evt)
{
    switch(evt->event_id)
    {
        case HTTP_EVENT_HEADER_SENT:
            // Free the existing response buffer before receiving a new response
            if (response_buffer != NULL) {
                free(response_buffer);
                response_buffer = NULL;
            }
            total_read_len = 0;
            break;
        case HTTP_EVENT_ON_DATA:
            // Reallocate the response buffer to hold the new data
            response_buffer = realloc(response_buffer, total_read_len + evt->data_len + 1);
            if (response_buffer == NULL) {
                printf("Failed to allocate memory for response buffer\n");
                total_read_len = 0;
                break;
            }

            // Copy the new data into the response buffer
            memcpy(response_buffer + total_read_len, evt->data, evt->data_len);
            total_read_len += evt->data_len;

            // Null-terminate the response buffer
            response_buffer[total_read_len] = '\0';
            break;
        default:
            break;
    }
    return ESP_OK;
}

void send_post_login_request(Attendance_Device_t *device) {
    esp_http_client_config_t config = {
        .url = "https://api-m.sandbox.paypal.com/v1/oauth2/token",
        .method = HTTP_METHOD_POST,
        .cert_pem = NULL,
        .event_handler = client_event_post_handler,
        .use_global_ca_store = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // Prepare the authorization header value (Basic Auth)
    char auth_header_value[256]; // Adjust size as needed
    char client_credentials[256]; // Adjust size based on client_id and client_secret length
    sprintf(client_credentials, "%s:%s", device->Client_id, device->Client_secret);
    size_t olen;
    int ret = mbedtls_base64_encode(NULL, 0, &olen, (const unsigned char *)client_credentials, strlen(client_credentials));
    unsigned char encoded_credentials[olen];
    ret = mbedtls_base64_encode(encoded_credentials, olen, &olen, (const unsigned char *)client_credentials, strlen(client_credentials));
    sprintf(auth_header_value, "Basic %s", encoded_credentials);
    esp_http_client_set_header(client, "Authorization", auth_header_value);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, "grant_type=client_credentials", strlen("grant_type=client_credentials"));
    // Perform the HTTP POST request
    esp_err_t err = esp_http_client_perform(client);
    // Cleanup
    esp_http_client_cleanup(client);
    Get_Token(device);
}

void send_create_invoice_request(Attendance_Device_t *device, int quantity[]){
    esp_http_client_config_t config = {
        .url = "https://api-m.sandbox.paypal.com/v1/invoicing/invoices",
        .method = HTTP_METHOD_POST,
        .cert_pem = NULL,
        .event_handler = client_event_post_handler,
        .use_global_ca_store = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    // Buffer for post data
    char post_data[512]; // Adjust size as needed
    // Start of the JSON string
    int offset = snprintf(post_data, sizeof(post_data), 
            "{\"merchant_info\": {\"business_name\": \"%s\"},"
            "\"items\": [", device->Business_name);
    // Append each product
    for (int i = 0; i < device->number_of_products; ++i) {
        // Skip products with a quantity of 0
        if (quantity[i] == 0) continue;

        // Calculate remaining buffer size
        size_t remaining = sizeof(post_data) - offset;
        if (remaining <= 0) break; // Check to avoid buffer overflow

        // Determine if a comma should be added
        char addComma = 0; // Flag to determine whether to add a comma
        for (int j = i + 1; j < device->number_of_products; ++j) {
            if (quantity[j] > 0) {
                addComma = 1; // Set flag if there's another product with quantity > 0
                break;
            }
        }

        // Append product details
        offset += snprintf(post_data + offset, remaining, 
                            "{\"name\": \"%s\", \"quantity\": %d, \"unit_price\": {\"currency\": \"USD\", \"value\": \"%.0f\"}}%s",
                            device->products[i].name, quantity[i], device->products[i].price, addComma ? ", " : "");
    }

    // Finish the JSON string
    if (sizeof(post_data) - offset > 0) {
        snprintf(post_data + offset, sizeof(post_data) - offset, "]}");
    }
    ESP_LOGI(TAG, "Post data: %s", post_data);
    char authHeader[256];
    snprintf(authHeader, sizeof(authHeader), "Bearer %s", device->token);
    esp_http_client_set_header(client, "Authorization", authHeader);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_err_t err = esp_http_client_perform(client);
    get_invoice_id(device);
    esp_http_client_cleanup(client);
}

void send_invoice_request(Attendance_Device_t *device) {
    char url[256]; // Ensure this buffer is large enough
    sprintf(url, "https://api-m.sandbox.paypal.com/v1/invoicing/invoices/%s/send", device->invoice_id);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .cert_pem = NULL,
        .event_handler = client_event_post_handler,
        .use_global_ca_store = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // Set Authorization header with Bearer token
    char authHeader[256]; // Ensure this buffer is large enough
    sprintf(authHeader, "Bearer %s", device->token);
    esp_http_client_set_header(client, "Authorization", authHeader);
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

void send_create_qr_request(Attendance_Device_t *device){
    char url[256]; // Ensure this buffer is large enough
    sprintf(url,"https://api-m.sandbox.paypal.com/v1/invoicing/invoices/%s/qr-code", device->invoice_id);
    ESP_LOGI(TAG,"URL: %s",url);
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .cert_pem = NULL,
        .event_handler = client_event_post_handler,
        .use_global_ca_store = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    // Set Authorization header with Bearer token
    char authHeader[256]; // Ensure this buffer is large enough
    sprintf(authHeader, "Bearer %s", device->token);
    esp_http_client_set_header(client, "Authorization", authHeader);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    const char* post_data = "{\"width\": 200, \"height\": 200}";
    ESP_LOGI(TAG,"Post data: %s",post_data);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_err_t err = esp_http_client_perform(client);
    Get_Image(device);
    esp_http_client_cleanup(client);    
}

void send_check_invoice_request(Attendance_Device_t *device){
    char url[256]; // Ensure this buffer is large enough
    sprintf(url,"https://api-m.sandbox.paypal.com/v1/invoicing/invoices/%s", device->invoice_id);
    ESP_LOGI(TAG,"URL: %s",url);
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .cert_pem = NULL,
        .event_handler = client_event_post_handler,
        .use_global_ca_store = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    // Set Authorization header with Bearer token
    char authHeader[256]; // Ensure this buffer is large enough
    sprintf(authHeader, "Bearer %s", device->token);
    esp_http_client_set_header(client, "Authorization", authHeader);
    esp_err_t err = esp_http_client_perform(client);
    get_invoice_status(device);
    esp_http_client_cleanup(client);    
}

void send_cancel_invoice_request(Attendance_Device_t *device){
    char url[256]; // Ensure this buffer is large enough
    sprintf(url,"https://api-m.sandbox.paypal.com/v1/invoicing/invoices/%s/cancel", device->invoice_id);
    ESP_LOGI(TAG,"URL: %s",url);
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .cert_pem = NULL,
        .event_handler = client_event_post_handler,
        .use_global_ca_store = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    char authHeader[256]; // Ensure this buffer is large enough
    sprintf(authHeader, "Bearer %s", device->token);
    esp_http_client_set_header(client, "Authorization", authHeader);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    const char* post_data = "{}";
    ESP_LOGI(TAG,"Post data: %s",post_data);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client); 
    device->invoice_status = true;
    device->invoice_id = NULL;
    device->image = NULL;
}

void generate_admin_html(Attendance_Device_t* device) {
    char html[2048*2];
    sprintf(html,
        "<body>"
        "<h1>ESP32 Web Server</h1>"
        "<h2>Device Config</h2>"
        "<div style=\"display: flex; justify-content: space-between;\">"
        "<div style=\"flex: 1; margin-left: 10px;\">"
        "<form method=\"POST\" action=\"/admin_config\">"
            "<label for=\"ssid\">Wifi ID:</label><br>"
            "<input type=\"text\" id=\"ssid\" name=\"ssid\" value=\"%s\"><br>"
            "<label for=\"password\">Wifi Password:</label><br>"
            "<input type=\"password\" id=\"password\" name=\"password\" value=\"%s\"><br>"
            "<label for=\"Client_id\">Client Id:</label><br>"
            "<input type=\"text\" id=\"Client_id\" name=\"Client_id\" value=\"%s\"><br>"
            "<lable for=\"Client_secret\">Client Secret:</label><br>"
            "<input type=\"text\" id=\"Client_secret\" name=\"Client_secret\" value=\"%s\"><br>"
            "<label for=\"Business_name\">Business Name:</label><br>"
            "<input type=\"text\" id=\"Business_name\" name=\"Business_name\" value=\"%s\"><br>"
            "<label for=\"number_of_products\">Number Of Products:</label><br>"
            "<input type=\"text\" id=\"number_of_products\" name=\"number_of_products\" value=\"%d\"><br>"
            "<label for=\"adminPass\">Admin Password:</label><br>"
            "<input type=\"password\" id=\"adminPass\" name=\"adminPass\" value=\"%s\"><br>"
            "<label for=\"supporterPass\">Supporter Password:</label><br>"
            "<input type=\"password\" id=\"supporterPass\" name=\"supporterPass\" value=\"%s\"><br>"
            "<input type=\"submit\" value=\"Submit\">"
        "</form>"
        "<form method=\"post\" action=\"/logout\">"
            "<input type=\"submit\" value=\"Get Back\">"
        "</form>"
        "</div>"
        "<div style=\"flex: 1; margin-right: 10px;\">"
        "<h3>Current Config:</h3>"
        "<p id=\"current_ssid\">SSID: %s</p>"
        "<p id=\"current_password\">Password: %s</p>"
        "<p id=\"current_ClientId\">Client Id: %s</p>"
        "<p id=\"current_ClientSecret\">Client Secret: %s</p>"
        "<p id=\"current_BusinessName\">Business Name: %s</p>"
        "<p id=\"current_NumberOfProducts\">Number Of Products: %d</p>"
        "<p id=\"current_adminPass\">Admin Password: %s</p>"
        "<p id=\"current_supporterPass\">Supporter Password: %s</p>"
        "</div>"
        "</div>"
        "</body>"
        "</html>",
        device->ssid, device->pass, device->Client_id, device->Client_secret, device->Business_name, device->number_of_products, device->adminPass, device->supporterPass,
        device->ssid, device->pass, device->Client_id, device->Client_secret, device->Business_name, device->number_of_products, device->adminPass, device->supporterPass
    );
    ESP_LOGI(TAG, "Generated HTML: %s", html);
    FILE *f = fopen("/storage/admin_config.html", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }
    fprintf(f, "%s", html);
    fclose(f);
}

void generate_auth_admin_header(char* username, char* admin_pass) {
    char credentials[64];
    sprintf(credentials, "%s:%s", username, admin_pass);
    unsigned char base64_output[128];
    size_t base64_output_length;
    mbedtls_base64_encode(base64_output, sizeof(base64_output), &base64_output_length, (unsigned char*)credentials, strlen(credentials));
    base64_output[base64_output_length] = '\0';
    sprintf(expected_auth_admin_header, "Basic %s", base64_output);
}

void generate_auth_supporter_header(char* username, char* supporter_pass) {
    char credentials[64];
    sprintf(credentials, "%s:%s", username, supporter_pass);
    unsigned char base64_output[128];
    size_t base64_output_length;
    mbedtls_base64_encode(base64_output, sizeof(base64_output), &base64_output_length, (unsigned char*)credentials, strlen(credentials));
    base64_output[base64_output_length] = '\0';
    sprintf(expected_auth_supporter_header, "Basic %s", base64_output);
}

void generate_supporter_html(Attendance_Device_t* device){
    char html[2048*2];
    sprintf(html,
        "<body>"
        "<h1>ESP32 Web Server</h1>"
        "<h2>Current Config</h2>"
        "<div style=\"display: flex; justify-content: space-between;\">"
        "<div style=\"flex: 1; margin-left: 10px;\">"
        "<p id=\"current_ssid\">Wifi ID: %s</p>"
        "<p id=\"current_password\">Wifi Password: %s</p>"
        "<p id=\"current_ClientId\">Client Id: %s</p>"
        "<p id=\"current_ClientSecret\">Client Secret: %s</p>"
        "<p id=\"current_BusinessName\">Business Name: %s</p>"
        "<p id=\"current_NumberOfProducts\">Number Of Products: %d</p>"
        "<p id=\"current_supporterPass\">Supporter Password: %s</p>"
        "</div>"
        "</body>"
        "</html>",
        device->ssid, device->pass, device->Client_id, device->Client_secret, device->Business_name, device->number_of_products, device->supporterPass);
    ESP_LOGI(TAG, "Generated HTML: %s", html);
    FILE *f = fopen("/storage/supporter_config.html", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }
    fprintf(f, "%s", html);
    fclose(f);
}

void generate_product_config_html(Attendance_Device_t* device) {
    char html[1024]; // Adjust size as needed
    // Start of the HTML document
    int offset = snprintf(html, sizeof(html),
        "<body>"
        "<h1>Product Configuration</h1>"
        "<ul>");

    // Assuming device->products is an array of a struct with name and value, and device->num_products is the count
    for (int i = 0; i < device->number_of_products; i++) {
        // Append each product as a list item
        offset += snprintf(html + offset, sizeof(html) - offset,
                           "<li>%s - %.0f</li>", device->products[i].name, device->products[i].price);
    }

    // End of the products list
    offset += snprintf(html + offset, sizeof(html) - offset, "</ul>");

    // Add a form for adding new products
    offset += snprintf(html + offset, sizeof(html) - offset,
        "<h2>Add New Product</h2>"
        "<form method=\"POST\" action=\"/product_config\">"
        "Name: <input type='text' name='name'><br>"
        "Price: <input type='number' step='0.01' name='price'><br>"
        "<input type='submit' value='Add Product'>"
        "</form>"
        "</body></html>");

    // Write the HTML content to a new file
    FILE *f = fopen("/storage/product_config.html", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open product_config.html for writing");
        return;
    }
    fprintf(f, "%s", html);
    fclose(f);
    ESP_LOGI(TAG, "Generated product_config.html");
}

char *urlDecode(const char *str) {
  //int d = 0; /* whether or not the string is decoded */

  char *dStr = (char *) malloc(strlen(str) + 1);
  char eStr[] = "00"; /* for a hex code */

  strcpy(dStr, str);

  //while(!d) {
    //d = 1;
    int i; /* the counter for the string */

    for(i=0;i<strlen(dStr);++i) {

      if(dStr[i] == '%') {
        if(dStr[i+1] == 0)
          return dStr;

        if(isxdigit((unsigned char)dStr[i+1]) && isxdigit((unsigned char)dStr[i+2])) {

          //d = 0;

          /* combine the next to numbers into one */
          eStr[0] = dStr[i+1];
          eStr[1] = dStr[i+2];

          /* convert it to decimal */
          long int x = strtol(eStr, NULL, 16);

          /* remove the hex */
          memmove(&dStr[i+1], &dStr[i+3], strlen(&dStr[i+3])+1);

          dStr[i] = x;
        }
      }
      else if(dStr[i] == '+') { dStr[i] = ' '; }
    }
  //}
  return dStr;
}

void get_config(Attendance_Device_t *device){
    FILE *f=fopen("/storage/ssid.txt","r");
    if (f==NULL){
        ESP_LOGI(TAG, "Failed to get ssid wifi from file");
    }
    else
    {
        char line[64];
        fgets(line,sizeof(line),f);
        fclose(f);
        device->ssid=strdup(line);
    }
    f=fopen("/storage/pass.txt","r");
    if(f==NULL){
        ESP_LOGI(TAG, "Failed to get password wifi from file");
    }
    else
    {
        char line[64];
        fgets(line,sizeof(line),f);
        fclose(f);
        device->pass=strdup(line);
    }
    f=fopen("/storage/Client_id.txt","r");
    if(f==NULL){
        ESP_LOGI(TAG, "Failed to get Device Id from file");
    }
    else
    {
        char line[256];
        fgets(line,sizeof(line),f);
        fclose(f);
        device->Client_id=strdup(line);
    }
    f=fopen("/storage/Client_secret.txt","r");
    if(f==NULL){
        ESP_LOGI(TAG, "Failed to Service Code from file");
    }
    else
    {
        char line[256];
        fgets(line,sizeof(line),f);
        fclose(f);
        device->Client_secret=strdup(line);
    }
    f=fopen("/storage/Business_name.txt","r");
    if(f==NULL){
        ESP_LOGI(TAG, "Failed to get Admin password from file");
    }
    else
    {
        char line[64];
        fgets(line,sizeof(line),f);
        fclose(f);
        device->Business_name=strdup(line);
    }
    f=fopen("/storage/number_of_products.txt","r");
    if(f==NULL){
        ESP_LOGI(TAG, "Failed to get Supporter password from file");
    }
    else
    {
        char line[64];
        fgets(line,sizeof(line),f);
        fclose(f);
        device->number_of_products=atoi(line);
    }  
    f=fopen("/storage/admin_pass.txt","r");
    if(f==NULL){
        ESP_LOGI(TAG, "Failed to get Admin password from file");
    }
    else
    {
        char line[64];
        fgets(line,sizeof(line),f);
        fclose(f);
        device->adminPass=strdup(line);
    }
    f=fopen("/storage/supporter_pass.txt","r");
    if(f==NULL){
        ESP_LOGI(TAG, "Failed to get Supporter password from file");
    }
    else
    {
        char line[64];
        fgets(line,sizeof(line),f);
        fclose(f);
        device->supporterPass=strdup(line);
    }
    f=fopen("/storage/products.txt","r");
    if(f==NULL){
        ESP_LOGI(TAG, "Failed to get products from file");
    }
    else
    {
        char line[256];
        int i=0;
        while(fgets(line,sizeof(line),f)){
            char *name=strtok(line,";");
            char *price=strtok(NULL,";");
            ESP_LOGI(TAG,"Name: %s",name);
            ESP_LOGI(TAG,"Price: %s",price);
            strncpy(device->products[i].name, name, sizeof(device->products[i].name) - 1);
            device->products[i].name[sizeof(device->products[i].name) - 1] = '\0'; // Ensure null-termination
            device->products[i].price=atoi(price);
            i++;
        }
        fclose(f);
    }
    device->ssid=urlDecode(device->ssid);
    device->pass=urlDecode(device->pass);
    device->Client_id=urlDecode(device->Client_id);
    device->Client_secret=urlDecode(device->Client_secret);
    device->Business_name=urlDecode(device->Business_name);
    device->adminPass=urlDecode(device->adminPass);
    device->supporterPass=urlDecode(device->supporterPass);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

esp_err_t redirect_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/choose_mode");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

httpd_uri_t redirect = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = redirect_get_handler,
};

/* An HTTP GET handler */
esp_err_t admin_config_get_handler(httpd_req_t *req)
{
    // Check if the Authorization header is present
    char auth_header[100]; // Adjust the size as needed
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK) {
        // The Authorization header is present. Check if it contains the correct credentials.
        // The credentials should be in the format "Basic base64(username:password)"
        // For "admin:admin", this is "Basic YWRtaW46YWRtaW4="
        ESP_LOGI(TAG, "Authorization header: %s!!!!!!!", auth_header);
        ESP_LOGI(TAG,"Expected Auth Admin Header: %s",expected_auth_admin_header);
        ESP_LOGI(TAG,"String compare: %d",strcmp(auth_header, expected_auth_admin_header));
        if (strcmp(auth_header, expected_auth_admin_header)==0) {
            generate_admin_html(device_to_use);
            // The credentials are correct. Handle the request...
            FILE* f = fopen("/storage/admin_config.html", "r");
            if (f == NULL) {
                ESP_LOGE(TAG, "Failed to open file for reading");
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                httpd_resp_send_chunk(req, line, strlen(line));
            }
            httpd_resp_send_chunk(req, NULL, 0); // Send empty chunk to signal end of response
            return ESP_OK;
        } else {
            // The credentials are incorrect. Send a 401 Unauthorized response with a WWW-Authenticate header.
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_set_hdr(req, "WWW-Authenticate","Basic realm=\"Access to the admin area.\",charset=\"UTF-8\"");
            httpd_resp_send(req, NULL, 0);
        }
    } else {
        // The Authorization header is not present. Send a 401 Unauthorized response with a WWW-Authenticate header.
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate","Basic realm=\"Access to the admin area.\",charset=\"UTF-8\"");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}
httpd_uri_t admin_config_uri = {
    .uri       = "/admin_config",
    .method    = HTTP_GET,
    .handler   = admin_config_get_handler,
    .user_ctx  = NULL
};

/* An HTTP POST handler for the /submit URI */
esp_err_t admin_config_submit_handler(httpd_req_t *req)
{
    char auth_header[100]; // Adjust the size as needed
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK) {
        // The Authorization header is present. Check if it contains the correct credentials.
        // The credentials should be in the format "Basic base64(username:password)"
        // For "admin:admin", this is "Basic YWRtaW46YWRtaW4="
        ESP_LOGI(TAG, "Authorization header: %s", auth_header);
        if (strcmp(auth_header, expected_auth_admin_header)!=0) {
            // The credentials are incorrect. Send a 401 Unauthorized response with a WWW-Authenticate header.
            httpd_resp_set_status(req, "401 not authorized");
            httpd_resp_set_hdr(req, "WWW-Authenticate","Basic realm=\"Access to the admin area.\",charset=\"UTF-8\"");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }
    char buf[1024];
    int ret, remaining = req->content_len;
    while (remaining > 0) {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }

        /* Process the data */
        ESP_LOGI(TAG, "Received %.*s", ret, buf);
        
        // Parse the received data
        char *ssid_start = strstr(buf, "ssid=");
        char *password_start = strstr(buf, "password=");
        char *admin_pass_start = strstr(buf, "adminPass=");
        char *supporter_pass_start = strstr(buf, "supporterPass=");
        char *client_id_start = strstr(buf, "Client_id=");
        char *client_secret_start = strstr(buf, "Client_secret=");
        char *business_name_start = strstr(buf, "Business_name=");
        char *number_of_products_start = strstr(buf, "number_of_products=");
        if (ssid_start && password_start && admin_pass_start && supporter_pass_start && client_id_start && client_secret_start && business_name_start && number_of_products_start) {
            ssid_start += 5;  // Skip past "ssid="
            password_start += 9;  // Skip past "password="
            admin_pass_start += 10;  // Skip past "adminPass="
            supporter_pass_start += 14;  // Skip past "supporterPass="
            client_id_start += 10;  // Skip past "Client_id="
            client_secret_start += 14;  // Skip past "Client_secret="
            business_name_start += 14;  // Skip past "Business_name="
            number_of_products_start += 19;  // Skip past "number_of_products="
            // Find the end of the ssid and password
            char *ssid_end = strchr(ssid_start, '&');
            char *password_end = strchr(password_start, '&');
            char *admin_pass_end = strchr(admin_pass_start, '&');
            char *supporter_pass_end = strchr(supporter_pass_start, '&');
            char *client_id_end = strchr(client_id_start, '&');
            char *client_secret_end = strchr(client_secret_start, '&');
            char *business_name_end = strchr(business_name_start, '&');
            char *number_of_products_end = strchr(number_of_products_start, '&');
            if (!ssid_end) ssid_end = buf + ret;
            if (!password_end) password_end = buf + ret;
            if (!admin_pass_end) admin_pass_end = buf + ret;
            if (!supporter_pass_end) supporter_pass_end = buf + ret;
            if (!client_id_end) client_id_end = buf + ret;
            if (!client_secret_end) client_secret_end = buf + ret;
            // Write the ssid to ssid.txt
            FILE *f = fopen("/storage/ssid.txt", "w");
            if (f) {
                size_t written = fwrite(ssid_start, 1, ssid_end - ssid_start, f);
                fclose(f);
                if (written != ssid_end - ssid_start) {
                    ESP_LOGE(TAG, "Failed to write SSID to ssid.txt");
                }
            } else {
                ESP_LOGE(TAG, "Failed to open ssid.txt for writing");
            }

            // Write the password to pass.txt
            f = fopen("/storage/pass.txt", "w");
            if (f) {
                size_t written = fwrite(password_start, 1, password_end - password_start, f);
                fclose(f);
                if (written != password_end - password_start) {
                    ESP_LOGE(TAG, "Failed to write password to pass.txt");
                }
            } else {
                ESP_LOGE(TAG, "Failed to open pass.txt for writing");
            }
            f=fopen("/storage/admin_pass.txt","w");
            if(f){
                size_t written = fwrite(admin_pass_start, 1, admin_pass_end - admin_pass_start, f);
                fclose(f);
                if(written != admin_pass_end - admin_pass_start){
                    ESP_LOGE(TAG, "Failed to write Admin password to admin_pass.txt");
                }
            } else {
                ESP_LOGE(TAG, "Failed to open admin_pass.txt for writing");
            }
            f=fopen("/storage/supporter_pass.txt","w");
            if(f){
                size_t written = fwrite(supporter_pass_start, 1, supporter_pass_end - supporter_pass_start, f);
                fclose(f);
                if(written != supporter_pass_end - supporter_pass_start){
                    ESP_LOGE(TAG, "Failed to write Supporter password to supporter_pass.txt");
                }
            } else {
                ESP_LOGE(TAG, "Failed to open supporter_pass.txt for writing");
            }
            f=fopen("/storage/Client_id.txt","w");
            if(f){
                size_t written = fwrite(client_id_start, 1, client_id_end - client_id_start, f);
                fclose(f);
                if(written != client_id_end - client_id_start){
                    ESP_LOGE(TAG, "Failed to write Client Id to Client_id.txt");
                }
            } else {
                ESP_LOGE(TAG, "Failed to open Client_id.txt for writing");
            }
            f=fopen("/storage/Client_secret.txt","w");
            if(f){
                size_t written = fwrite(client_secret_start, 1, client_secret_end - client_secret_start, f);
                fclose(f);
                if(written != client_secret_end - client_secret_start){
                    ESP_LOGE(TAG, "Failed to write Client Secret to Client_secret.txt");
                }
            } else {
                ESP_LOGE(TAG, "Failed to open Client_secret.txt for writing");
            }
            f=fopen("/storage/Business_name.txt","w");
            if(f){
                size_t written = fwrite(business_name_start, 1, business_name_end - business_name_start, f);
                fclose(f);
                if(written != business_name_end - business_name_start){
                    ESP_LOGE(TAG, "Failed to write Business Name to Business_name.txt");
                }
            } else {
                ESP_LOGE(TAG, "Failed to open Business_name.txt for writing");
            }
            f=fopen("/storage/number_of_products.txt","w");
            if(f){
                ESP_LOGI(TAG,"Number of products: %s",number_of_products_start);
                ESP_LOGI(TAG,"Number of products end: %s",number_of_products_end);
                size_t written = fwrite(number_of_products_start, 1, number_of_products_end - number_of_products_start, f);
                fclose(f);
                if(written != number_of_products_end - number_of_products_start){
                    ESP_LOGE(TAG, "Failed to write Number Of Products to number_of_products.txt");
                }
            } else {
                ESP_LOGE(TAG, "Failed to open number_of_products.txt for writing");
            }       
        } else {
            ESP_LOGE(TAG, "Failed to parse SSID and password from received data");
        }
        remaining -= ret;
    }
    /* Send a response */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/choose_mode");
    httpd_resp_send(req, NULL, 0);
    vTaskDelay(1000/ portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}
httpd_uri_t admin_submit_uri = {
    .uri       = "/admin_config",
    .method    = HTTP_POST,
    .handler   = admin_config_submit_handler,
    .user_ctx  = NULL
};

esp_err_t supporter_config_get_handler(httpd_req_t *req){
    char auth_header[100]; // Adjust the size as needed
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK) {
        // The Authorization header is present. Check if it contains the correct credentials.
        // The credentials should be in the format "Basic base64(username:password)"
        // For "admin:admin", this is "Basic YWRtaW46YWRtaW4="
        ESP_LOGI(TAG, "Authorization header: %s", auth_header);
        if (strcmp(auth_header, expected_auth_supporter_header)!=0) {
            // The credentials are incorrect. Send a 401 Unauthorized response with a WWW-Authenticate header.
            httpd_resp_set_status(req, "401 not authorized");
            httpd_resp_set_hdr(req, "WWW-Authenticate","Basic realm=\"Access to the supporter area.\",charset=\"UTF-8\"");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }
    generate_supporter_html(device_to_use);
    FILE* f = fopen("/storage/supporter_config.html", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_send_chunk(req, line, strlen(line));
    }
    httpd_resp_send_chunk(req, NULL, 0); // Send empty chunk to signal end of response
    return ESP_OK;
}

httpd_uri_t supporter_config_get_uri = {
    .uri       = "/supporter_config",
    .method    = HTTP_GET,
    .handler   = supporter_config_get_handler,
    .user_ctx  = NULL
};
// This is the handler for the login page
esp_err_t choose_mode_get_handler(httpd_req_t *req) {
    FILE* f = fopen("/storage/choose_mode.html", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_send_chunk(req, line, strlen(line));
    }
    httpd_resp_send_chunk(req, NULL, 0); // Send empty chunk to signal end of response

    fclose(f);
    return ESP_OK;
}

// This is the handler for the login request
esp_err_t choose_mode_post_handler(httpd_req_t *req) {
    char buf[100]; // Adjust the size as needed
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    ESP_LOGI(TAG, "Received data: %s", buf);
    if (strcmp(buf, "Admin") == 0) {
        // If the Admin button was clicked, redirect to the admin_config page...
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/admin_config");
        httpd_resp_send(req, NULL, 0);
    } else if (strcmp(buf, "Support") == 0) {
        // If the Support button was clicked, redirect to the supporter_config page...
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/supporter_config");
        httpd_resp_send(req, NULL, 0);
    } else if (strcmp(buf, "products")==0){
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/product_config");
        httpd_resp_send(req, NULL, 0);
    }
    else {
        // If neither button was clicked, send a 400 Bad Request response.
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}

httpd_uri_t choose_mode_get_uri = {
    .uri       = "/choose_mode",
    .method    = HTTP_GET,
    .handler   = choose_mode_get_handler,
    .user_ctx  = NULL
};

esp_err_t product_config_get_handler(httpd_req_t *req){
    generate_product_config_html(device_to_use);
    FILE* f = fopen("/storage/product_config.html", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_send_chunk(req, line, strlen(line));
    }
    httpd_resp_send_chunk(req, NULL, 0); // Send empty chunk to signal end of response
    fclose(f);
    return ESP_OK;
}

httpd_uri_t product_config_get_uri={
    .uri       = "/product_config",
    .method    = HTTP_GET,
    .handler   = product_config_get_handler,
    .user_ctx  = NULL
};

esp_err_t product_config_post_handler(httpd_req_t *req){
    char buf[100]; // Adjust the size as needed
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    ESP_LOGI(TAG, "Received data: %s", buf);
    char *name_start = strstr(buf, "name=");
    char *price_start = strstr(buf, "price=");
    if (name_start && price_start) {
        name_start += 5;  // Skip past "name="
        price_start += 6;  // Skip past "price="
        // Find the end of the name and price
        char *name_end = strchr(name_start, '&');
        char *price_end = strchr(price_start, '&');
        if (!name_end) name_end = buf + ret;
        if (!price_end) price_end = buf + ret;

        // Temporarily null-terminate the name and price for safe string operations
        char tempName = *name_end;
        char tempPrice = *price_end;
        *name_end = '\0';
        *price_end = '\0';

        // Write the product to products.txt
        FILE *f = fopen("/storage/products.txt", "a");
        if (f) {
            fprintf(f, "\n%s;%s", name_start, price_start);
            fclose(f);
        } else {
            ESP_LOGE(TAG, "Failed to open products.txt for writing");
        }
        *name_end = tempName;
        *price_end = tempPrice;
        f = fopen("/storage/number_of_products.txt", "w");
        if (f) {
            // Convert the number of products plus one to a string
            char numProductsStr[20]; // Ensure this buffer is large enough
            int numProducts = device_to_use->number_of_products + 1;
            snprintf(numProductsStr, sizeof(numProductsStr), "%d", numProducts);

            // Write the string to the file
            size_t written = fwrite(numProductsStr, 1, strlen(numProductsStr), f);
            fclose(f);

            if (written != strlen(numProductsStr)) {
                ESP_LOGE(TAG, "Failed to write Number Of Products to number_of_products.txt");
            }
        } else {
            ESP_LOGE(TAG, "Failed to open number_of_products.txt for writing");
        }
    } else {
        ESP_LOGE(TAG, "Failed to parse product name and price from received data");
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/choose_mode");
    httpd_resp_send(req, NULL, 0);
    esp_restart();
    return ESP_OK;

}

httpd_uri_t product_config_post_uri={
    .uri       = "/product_config",
    .method    = HTTP_POST,
    .handler   = product_config_post_handler,
    .user_ctx  = NULL
};

// Register the login request handler
httpd_uri_t choose_mode_post_uri = {
    .uri       = "/choose_mode",
    .method    = HTTP_POST,
    .handler   = choose_mode_post_handler,
};

esp_err_t logout_handler(httpd_req_t *req)
{
    // Then, send a 303 See Other status to redirect the client to choose_mode
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/choose_mode");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

httpd_uri_t logout_uri = {
    .uri       = "/logout",
    .method    = HTTP_POST,
    .handler   = logout_handler,
};
void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 20480; // Increase the stack size
    config.max_uri_handlers=15;
    // Start the httpd server
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    // Register URI handlers

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &admin_config_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &admin_submit_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &choose_mode_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &choose_mode_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &redirect));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &logout_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &supporter_config_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &product_config_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &product_config_post_uri));
}
static void event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool wifi_init_sta_ap(Attendance_Device_t *device) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // Initialize the WiFi library
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Configure the WiFi interface in STA mode
    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid = "",
            .password = "",
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    strncpy((char *)wifi_sta_config.sta.ssid, device->ssid, sizeof(wifi_sta_config.sta.ssid));
    strncpy((char *)wifi_sta_config.sta.password, device->pass, sizeof(wifi_sta_config.sta.password));

    // Configure the WiFi interface in AP mode
    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    // Set the WiFi mode to AP+STA and configure the interfaces
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_ap_config));

    // Start the WiFi interfaces
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for the connection to the AP to be established
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", device->ssid, device->pass);
        device->connected=true;
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", device->ssid, device->pass);
        device->connected=false;
        return false;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        device->connected=false;
        return false;
    }
    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

void Start_connection(Attendance_Device_t *device)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    get_config(device);
    device_to_use = device;
    generate_auth_admin_header("admin",device->adminPass);
    generate_auth_supporter_header("supporter",device->supporterPass);
    if (wifi_init_sta_ap(device)) {
        ESP_LOGI(TAG, "Connected to WiFi network");
    } else {
        ESP_LOGE(TAG, "Failed to connect to WiFi network: %s",esp_err_to_name(ret));
    }
    ESP_LOGI(TAG,"Hosting at 192.168.4.1 now!");
    start_webserver();
}
