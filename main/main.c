#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <freertos/portmacro.h>
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <esp_intr_alloc.h>

#include "st7789.h"
#include "decode_png.h"
#include "pngle.h"
#include "mbedtls/base64.h"
#include "HD44780.c"
#include "driver/gpio.h"
#include "wifi.h"

#define INTERVAL 400
#define WAIT vTaskDelay(INTERVAL)
#define LCD_ADDR                    0x27
#define SDA_PIN                     21
#define SCL_PIN                     22
#define LCD_COLS                    20
#define LCD_ROWS                    4
#define BUTTON1_GPIO_PIN            32
#define BUTTON2_GPIO_PIN            33
#define BUTTON3_GPIO_PIN            14
#define BUTTON4_GPIO_PIN            15
#define GPIO_INPUT_PIN_SEL          ((1ULL << BUTTON1_GPIO_PIN) | (1ULL << BUTTON2_GPIO_PIN) | (1ULL << BUTTON3_GPIO_PIN) | (1ULL << BUTTON4_GPIO_PIN))
#define DEBOUNCE_TIME_MS            50

#define STATE_0                     0   // Initial state
#define STATE_1                     1   // Product and quantity selected
#define STATE_2                     2   // Total payment and confirm
#define STATE_3                     3   // Create QR
#define STATE_4                     4   // Display QR and waiting payment status
#define STATE_5                     5   // Checking payment process
#define STATE_6                     6   // cancel payment
#define STATE_RESET                 7   // reset state

// Queue handle for button interrupts
//static QueueHandle_t interputQueue = NULL;
// Semaphore for button press
static SemaphoreHandle_t buttonSemaphore = NULL; 
static const char *TAG = "DEVICE";

int cart[100];
uint32_t total_payment = 0;
int product_index = 0;
int previous_product_index = 0;
int quantity = 0;
char file[1000];

int8_t current_state = STATE_0;
int8_t previous_state = STATE_0;
int8_t next_state = STATE_0;

bool timeout = false;
bool success = false;

Attendance_Device_t device;

void setup_button();
void LCD_Task(void *param);
void action_state(void* arg);
void button_task(void* arg);

static void IRAM_ATTR button_isr_handler(void *args);
void spiffs_init();
TickType_t PNGTest(TFT_t * dev, char * file, int width, int height);
void ST7789(void *pvParameters);


// You have to set these CONFIG value using menuconfig.
#if 0
#define CONFIG_WIDTH  240
#define CONFIG_HEIGHT 240
#define CONFIG_MOSI_GPIO 23
#define CONFIG_SCLK_GPIO 18
#define CONFIG_CS_GPIO -1
#define CONFIG_DC_GPIO 19
#define CONFIG_RESET_GPIO 15
#define CONFIG_BL_GPIO -1
#endif

void app_main(void)
{
	// Set up
    LCD_init(LCD_ADDR, SDA_PIN, SCL_PIN, LCD_COLS, LCD_ROWS);
    setup_button();
    // products[0] = (Product){"Product 1", 10.0};
    // products[1] = (Product){"Product 2", 20.0};
    // products[2] = (Product){"Product 3", 30.0};
    // products[3] = (Product){"Product 4", 40.0}; 
    // products[4] = (Product){"Product 5", 50.0};
	spiffs_init();
	Start_connection(&device);
	send_post_login_request(&device);
	// cart[0] = 1;
	// cart[1] = 2;
	// cart[2] = 3;
	// cart[3] = 4;
	// cart[4] = 0;
	// Create tasks
	xTaskCreate(ST7789, "ST7789", 1024*6, NULL, 2, NULL);
    vTaskDelay(2000/ portTICK_PERIOD_MS);
    xTaskCreate(&LCD_Task, "LCD_DemoTask", 2048, NULL, 5, NULL);
    xTaskCreate(&button_task, "button_task", 2048, NULL, 10, NULL);
    xTaskCreate(&action_state, "action_state", 4096, NULL, 10, NULL);
    for (int i=0; i < device.number_of_products; i++) 
        cart[i] = 1;
}

void spiffs_init()
{
    esp_vfs_spiffs_conf_t config={
        .base_path="/storage",
        .partition_label=NULL,
        .max_files=5,
        .format_if_mount_failed=true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&config);

    if (ret!=ESP_OK){
        ESP_LOGI(TAG, "Failed to intialize SPIFFS (%s)",esp_err_to_name(ret));
        return;
    }

    size_t total=0, used=0;
    ret = esp_spiffs_info(config.partition_label,&total,&used);
    if(ret!=ESP_OK)
    {
        ESP_LOGI(TAG, "Failed to get SPIFFS partition information (%s)",esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d",total,used);
    }
}

TickType_t PNGTest(TFT_t * dev, char * file, int width, int height) {
	TickType_t startTick, endTick, diffTick;
	startTick = xTaskGetTickCount();

	lcdSetFontDirection(dev, 0);
	lcdFillScreen(dev, BLACK);

	// open PNG file
	FILE* fp = fopen(file, "rb");
	if (fp == NULL) {
		ESP_LOGW(__FUNCTION__, "File not found [%s]", file);
		return 0;
	}

	char buf[1024];
	size_t remain = 0;
	int len;

	pngle_t *pngle = pngle_new(width, height);

	pngle_set_init_callback(pngle, png_init);
	pngle_set_draw_callback(pngle, png_draw);
	pngle_set_done_callback(pngle, png_finish);

	double display_gamma = 2.2;
	pngle_set_display_gamma(pngle, display_gamma);


	while (!feof(fp)) {
		if (remain >= sizeof(buf)) {
			ESP_LOGE(__FUNCTION__, "Buffer exceeded");
			while(1) vTaskDelay(1);
		}

		len = fread(buf + remain, 1, sizeof(buf) - remain, fp);
		if (len <= 0) {
			//printf("EOF\n");
			break;
		}

		int fed = pngle_feed(pngle, buf, remain + len);
		if (fed < 0) {
			ESP_LOGE(__FUNCTION__, "ERROR; %s", pngle_error(pngle));
			while(1) vTaskDelay(1);
		}

		remain = remain + len - fed;
		if (remain > 0) memmove(buf, buf + fed, remain);
	}

	fclose(fp);

	uint16_t _width = width;
	uint16_t _cols = 0;
	if (width > pngle->imageWidth) {
		_width = pngle->imageWidth;
		_cols = (width - pngle->imageWidth) / 2;
	}
	ESP_LOGD(__FUNCTION__, "_width=%d _cols=%d", _width, _cols);

	uint16_t _height = height;
	uint16_t _rows = 0;
	if (height > pngle->imageHeight) {
			_height = pngle->imageHeight;
			_rows = (height - pngle->imageHeight) / 2;
	}
	ESP_LOGD(__FUNCTION__, "_height=%d _rows=%d", _height, _rows);
	uint16_t *colors = (uint16_t*)malloc(sizeof(uint16_t) * _width);

#if 0
	for(int y = 0; y < _height; y++){
		for(int x = 0;x < _width; x++){
			pixel_png pixel = pngle->pixels[y][x];
			uint16_t color = rgb565(pixel.red, pixel.green, pixel.blue);
			lcdDrawPixel(dev, x+_cols, y+_rows, color);
		}
	}
#endif

	for(int y = 0; y < _height; y++){
		for(int x = 0;x < _width; x++){
			//pixel_png pixel = pngle->pixels[y][x];
			//colors[x] = rgb565(pixel.red, pixel.green, pixel.blue);
			colors[x] = pngle->pixels[y][x];
		}
		lcdDrawMultiPixels(dev, _cols, y+_rows, _width, colors);
		vTaskDelay(1);
	}
	lcdDrawFinish(dev);
	free(colors);
	pngle_destroy(pngle, width, height);
	endTick = xTaskGetTickCount();
	diffTick = endTick - startTick;
	ESP_LOGI(__FUNCTION__, "elapsed time[ms]:%"PRIu32,diffTick*portTICK_PERIOD_MS);
	return diffTick;
}

void ST7789(void *pvParameters)
{
    TFT_t dev;
    int count = 0;
    spi_master_init(&dev, CONFIG_MOSI_GPIO, CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, CONFIG_DC_GPIO, CONFIG_RESET_GPIO, CONFIG_BL_GPIO);
	lcdInit(&dev, CONFIG_WIDTH, CONFIG_HEIGHT, CONFIG_OFFSETX, CONFIG_OFFSETY);	
	while (1)
	{   
        bool cancel = false;
		strcpy(file, "/storage/idle.png");
		PNGTest(&dev, file, CONFIG_WIDTH, CONFIG_HEIGHT);
		while (!device.image)
		{
			vTaskDelay(1000 / portTICK_PERIOD_MS);
		}
        next_state = STATE_4;
		char* encoded_image = device.image;
		size_t olen = 0;
		uint8_t decoded_image[1000]; // Adjust this size as needed
		mbedtls_base64_decode(decoded_image, sizeof(decoded_image), &olen, (unsigned char *)encoded_image, strlen(encoded_image));	
		ESP_LOGI(TAG, "Base64 decoded image size: %d", olen);
		ESP_LOGI(TAG, "Base64 decoded image: %s", decoded_image);
		FILE *f= fopen("/storage/qr.png", "w");
		fwrite(decoded_image, 1, olen, f);
		fclose(f);
		strcpy(file, "/storage/qr.png");
		PNGTest(&dev, file, CONFIG_WIDTH, CONFIG_HEIGHT);
		while(!device.invoice_status)
		{
            count++;
			send_check_invoice_request(&device);
            if (current_state==STATE_6)
            {
                send_cancel_invoice_request(&device);
                strcpy(file, "/storage/idle.png");
                PNGTest(&dev, file, CONFIG_WIDTH, CONFIG_HEIGHT);
                vTaskDelay(5000 / portTICK_PERIOD_MS);
                next_state = STATE_RESET;
                cancel = true;
            }
            if (count == 50)
            {
                send_cancel_invoice_request(&device);
                timeout = true;
                next_state = STATE_5;
                count = 0;
            }
			vTaskDelay(1000 / portTICK_PERIOD_MS);
		}
        //next_state=STATE_7;
        // while(current_state!=STATE_Final)
        // {
        //     vTaskDelay(1000 / portTICK_PERIOD_MS);
        // }
        // strcpy(file, "/storage/idle.png");
        // PNGTest(&dev, file, CONFIG_WIDTH, CONFIG_HEIGHT);
        success = true;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        next_state = STATE_5;
	}
}

static void IRAM_ATTR button_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(buttonSemaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void setup_button() 
{

    buttonSemaphore = xSemaphoreCreateBinary();
    if (buttonSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    gpio_config_t button_config = 
    {
        .pin_bit_mask = GPIO_INPUT_PIN_SEL,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config(&button_config);

   // interputQueue = xQueueCreate(10, sizeof(uint32_t));

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON1_GPIO_PIN, button_isr_handler, (void*)BUTTON1_GPIO_PIN);
    gpio_isr_handler_add(BUTTON2_GPIO_PIN, button_isr_handler, (void*)BUTTON2_GPIO_PIN);
    gpio_isr_handler_add(BUTTON3_GPIO_PIN, button_isr_handler, (void*)BUTTON3_GPIO_PIN);
    gpio_isr_handler_add(BUTTON4_GPIO_PIN, button_isr_handler, (void*)BUTTON4_GPIO_PIN);
}

void button_task(void* arg)
{
    //uint32_t pinNumber;
    while(1) 
    {
        if (xSemaphoreTake(buttonSemaphore, portMAX_DELAY) == pdTRUE) 
        {
            // Debounce delay
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME_MS));

            // Check which button was pressed
            // Button 1
            if (gpio_get_level(BUTTON1_GPIO_PIN) == 0) 
            {
                // Log
                ESP_LOGI(TAG, "Button 1 pressed");
                // Handle Button 1 press
                switch(current_state)
                {
                    case STATE_0:
                        // Log
                        ESP_LOGI(TAG, "Switching state_1");
                        // Set next_state
                        next_state = STATE_1;
                        break;
                    case STATE_1:
                        // Log
                        ESP_LOGI(TAG, "Switching state_2");
                        // Set next_state
                        next_state = STATE_2;
                        for(int i = 0; i < device.number_of_products; i++)
                            total_payment += cart[i] * device.products[i].price; 
                        break;
                    case STATE_2:
                        // Log
                        ESP_LOGI(TAG, "Switching state_3");
                        // Set next_state
                        next_state = STATE_3;
                        break;
                    default:
                        break;
                }
            }
            // Button 2
            else if (gpio_get_level(BUTTON2_GPIO_PIN) == 0) 
            {
                ESP_LOGI(TAG, "Button 2 pressed");
                // Handle Button 2 press
                switch(current_state)
                {
                    case STATE_1:
                        //log
                        ESP_LOGI(TAG, "Switching product");
                        //change product
                        if (product_index == device.number_of_products-1)
                            product_index = 0;
                        else   
                            product_index++;
                        ESP_LOGI(TAG, "%d", product_index);
                        //LCD_clearScreen();
                        break;
                    case STATE_2:
                        //log
                        ESP_LOGI(TAG, "Return state 1");
                        //return state 1
                        next_state = STATE_1;
                        break;
                    case STATE_4:
                        //log
                        ESP_LOGI(TAG, "Cancel transaction");
                        //reset
                        next_state = STATE_6;
                        break;
                    default:
                        break;
                }
            } 
            //button 3
            else if (gpio_get_level(BUTTON3_GPIO_PIN) == 0) 
            {
                ESP_LOGI(TAG, "Button 3 pressed");
                // Handle Button 3 press
                switch(current_state)
                {
                    case STATE_1:
                        //log
                        ESP_LOGI(TAG, "Increase quantity");
                        // tang so luong
                        cart[product_index]++;
                        LCD_clearScreen();
                        break;
                    default:
                        break;
                }
            }
            //button 4
            else if (gpio_get_level(BUTTON4_GPIO_PIN) == 0)
            {
                ESP_LOGI(TAG, "Button 4 pressed");
                // Handle Button 4 press
                switch(current_state)
                {
                    case STATE_1:
                        //log
                        ESP_LOGI(TAG, "Decrease quantity");
                        cart[product_index] = (cart[product_index] = 0) ? 0 : cart[product_index] - 1 ;
                        LCD_clearScreen();
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

void LCD_Task(void *param)
{
    char num[20];
    while (true) 
    {
        if(current_state != previous_state)
        {
            LCD_clearScreen();
            previous_state = current_state;
        }
        switch(current_state)
        {
            case STATE_0:
                LCD_setCursor(6,0);
                LCD_writeStr("WELCOME");
                LCD_setCursor(1,2);
                LCD_writeStr("[1]  [2]  [3]  [4]");
                LCD_setCursor(0,3);
                LCD_writeStr("Start");
                break;
            case STATE_1:
                if (product_index != previous_product_index)
                {
                    LCD_clearScreen();
                    previous_product_index = product_index;
                }
                LCD_setCursor(7, 0);
                LCD_writeStr(device.products[product_index].name);
                LCD_setCursor(0, 1);
                sprintf(num, "%.1f", device.products[product_index].price);
                LCD_writeStr(num);
                LCD_setCursor(17,1);
                LCD_writeStr("x");
                LCD_setCursor(18,1);
                sprintf(num, "%d", cart[product_index]);
                LCD_writeStr(num);
                LCD_setCursor(1,2);
                LCD_writeStr("[1]  [2]  [3]  [4]");
                LCD_setCursor(1,3);
                LCD_writeStr("Done Next Inc  Dec");
                break;
            case STATE_2:
                LCD_setCursor(7,0);
                LCD_writeStr("Total");
                LCD_setCursor(5,1);
                sprintf(num, "%ld", total_payment);
                LCD_writeStr(num);
                LCD_setCursor(17,1);
                LCD_writeStr("USD");
                LCD_setCursor(1,2);
                LCD_writeStr("[1]  [2]  [3]  [4]");
                LCD_setCursor(1,3);
                LCD_writeStr("Pay  Back");
                break;
            case STATE_3:
                LCD_setCursor(3,1);
                LCD_writeStr("Waiting for QR");
                break;
            case STATE_4:
                LCD_setCursor(6,0);
                LCD_writeStr("Scan QR");
                LCD_setCursor(1,2);
                LCD_writeStr("[1]  [2]  [3]  [4]");
                LCD_setCursor(5,3);
                LCD_writeStr("Cancel");
                break;
            case STATE_5:
                if (timeout) 
                {
                    LCD_setCursor(8,1);
                    LCD_writeStr("Fail");
                    LCD_setCursor(6,2);
                    LCD_writeStr("Try again");
                }
                if (success)
                {
                    LCD_setCursor(6,1);
                    LCD_writeStr("Success");
                    LCD_setCursor(5,2);
                    LCD_writeStr("Thank you");
                }
                break;
            case STATE_6:
                LCD_setCursor(3,1);
                LCD_writeStr("Cancel invoice");
                break;
            case STATE_RESET:
                LCD_setCursor(3,1);
                LCD_writeStr("See you again");
            default:
                break;
        }
        vTaskDelay(50 / portTICK_PERIOD_MS); // 50ms delay
    }
}

void action_state(void* arg)
{
    while(1)
    {   
        // Change state
        current_state = next_state;

        switch(current_state)
        {
            // SEND REQUEST AND WAIT FOR QR CODE FROM PAYPAL
            case STATE_3:
                // Log
                ESP_LOGI(TAG, "send request to server and wait for QR code");
                send_create_invoice_request(&device, cart);
                send_invoice_request(&device);
                send_create_qr_request(&device);
                next_state = STATE_4;
                break;
            case STATE_5:
                vTaskDelay(5000 / portTICK_PERIOD_MS);
                next_state = STATE_RESET;
                break;
            case STATE_RESET:
                // Log
                ESP_LOGI(TAG, "reset index and return to state 0");
                // Reset product_index
                product_index = 0;
                // Reset cart
                // for(int i = 0; i < device.number_of_products;i++) 
                //     cart[i] = 0;
                // Reset total_payment
                total_payment = 0;
                // reset flag
                timeout = false;
                success = false;
                // 3s delay
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                // Set next_state
                next_state = STATE_0;
                break;
            default:  
                break;
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}