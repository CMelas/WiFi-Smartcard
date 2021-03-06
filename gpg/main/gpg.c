/* 
 * The main file for the ESP32
 * 
 * Handles:
 *    System initialization (storage)
 *    Input interrupts and Output (Buttons and LEDs)
 *    WiFi
 *    Connection with another machine
 *    Receiving APDU commands and writing APDU responses
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "rom/uart.h"
#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#include "esp_vfs.h"
#include "esp_vfs_fat.h"

#include "netlist.h"
#include "libAPDU.h"

#define PORT 5511       // The default port of this protocol
// Display the time it takes to complete an operation
//#define TIMING        // Do not enable unless testing, or a 1 second delay will be added for each operation
#define PRINTAPDU       // If defined, APDU info is printed, mainly used for debug reasons
#define PROCEEDBTN      // Do not perform a security operation until the button is pressed

// FreeRTOS event group to signal connected & ready to make a request
static EventGroupHandle_t wifiEventGroup;

// Flag that is set when connected to an AP with an IP
const int CONNECTED_BIT = BIT0;

// Handle of the wear levelling library instance
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// Mount path for the partition
const char *base_path = "/spiflash";

uint8_t connected = 0;  // Status bit for the WiFi
uint8_t proceed = 0;    // When the proceed button is pressed, proceed is set
uint8_t hardRst = 0;    // When the hard reset button is pressed, hardRst is set
uint16_t timeCount = 0; // How many operations were performed
uint64_t totalTime = 0; // Total time taken for all operations in milliseconds

void proceedHandle(void* arg) {     // Interrupt handler for the proceed button
    proceed = 1;
}

void hardReset(void* arg) {         // Interrupt handler for the hard reset button
    hardRst = 1;
}

uint8_t mountFS() {     // Mount the filesystem at the beginning
    static const char *TAG = "mountFS";
    ESP_LOGI(TAG, "Mounting FAT filesystem");
    const esp_vfs_fat_mount_config_t mount_config = {   // Mount the filesystem
            .max_files = 4,
            .format_if_mount_failed = true
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount(base_path, "storage", &mount_config, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (0x%x)", err);
        return 0;
    }
    return 1;
}

void unmountFS() {      // Unmount the filesystem before restarting
    static const char *TAG = "unmountFS";
    ESP_LOGI(TAG, "Unmounting FAT filesystem");
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_unmount(base_path, s_wl_handle));
    ESP_LOGI(TAG, "Done");
}

void initGPIO() {
    gpio_set_direction(GPIO_NUM_25, GPIO_MODE_OUTPUT);      // WiFi status LED
    gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT);      // Processing status LED

    gpio_set_direction(GPIO_NUM_12, GPIO_MODE_INPUT);       // Proceed button
    gpio_set_intr_type(GPIO_NUM_12, GPIO_INTR_POSEDGE);     // Interrupt on rising edge
    gpio_set_pull_mode(GPIO_NUM_12, GPIO_PULLDOWN_ONLY);    // Enable pull-down (spared a 10k resistor)
    gpio_install_isr_service(0);                            // Install GPIO interrupt service
    gpio_isr_handler_add(GPIO_NUM_12, proceedHandle, (void*) GPIO_NUM_12);  // Hook ISR handler

    gpio_set_direction(GPIO_NUM_17, GPIO_MODE_INPUT);       // Re-initialize button
    gpio_set_intr_type(GPIO_NUM_17, GPIO_INTR_POSEDGE);     // Interrupt on rising edge
    gpio_set_pull_mode(GPIO_NUM_17, GPIO_PULLDOWN_ONLY);    // Enable pull-down (spared a 10k resistor)
    gpio_isr_handler_add(GPIO_NUM_17, hardReset, (void*) GPIO_NUM_17);      // Hook ISR handler
}

void initNVS() {        // Initialize the non-volatile storage
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        const esp_partition_t* nvs_partition = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
        assert(nvs_partition && "partition table must have an NVS partition");
        ESP_ERROR_CHECK(esp_partition_erase_range(nvs_partition, 0, nvs_partition->size));
        // Retry nvs_flash_init
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static esp_err_t event_handler(void *ctx, system_event_t *event) {
    static const char *TAG = "wifiEventHandler";

    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        currNet = nextNet;      // Select the next network in the list and try to connect
        ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", (*wifiConfig[currNet]).sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, wifiConfig[currNet]));
        nextNet = (nextNet + 1) % NUMOFNETS;    // Prepare the next network to select
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        connected = 1;
        invalidate();                   // Invalidate / PIN Reset at a new WiFi connection
        gpio_set_level(GPIO_NUM_26, 1); // Connected to a network, light up the LED
        ESP_LOGI(TAG, "Connected to AP");
        xEventGroupSetBits(wifiEventGroup, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        connected = 0;
        invalidate();                   // Invalidate / PIN Reset at a WiFi disconnect
        gpio_set_level(GPIO_NUM_26, 0); // Disconnected, turn of the LED
        xEventGroupClearBits(wifiEventGroup, CONNECTED_BIT);
        currNet = nextNet;      // Select the next network in the list and try to connect
        ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", (*wifiConfig[nextNet]).sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, wifiConfig[nextNet]));
        nextNet = (nextNet + 1) % NUMOFNETS;    // Prepare the next network to select
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initWiFi(void) {    // Configure and initialize WiFi
    nextNet = 0;        // Attempt to connect to this network next

    tcpip_adapter_init();   // Initialize the TCP/IP adapter
    wifiEventGroup = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static void taskConnect(void *pvParameters) {
    static const char *TAG = "taskConnect";

    int sockfd, r;
    apdu_t comAPDU;
    outData output;
    char recvBuf[1024];
    struct sockaddr_in serv_addr;

    nvs_handle nvsHandle;       // Open NVS to check if the device has been initialized
    uint8_t initialized = 0;    // Flag to check
    gpio_set_level(GPIO_NUM_25, 1);     // Initialize/restore start
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvsHandle); // Open the NVS
    if (err != ESP_OK) {
        goto exit;
    } else {
        err = nvs_get_u8(nvsHandle, "initialized", &initialized);   // Get the initialized value
        nvs_close(nvsHandle);
        switch (err) {
            case ESP_OK:        // If it has already been initialized, restore the data
                if (restoreState() != 0) {  // If something went wrong, then restart
                    hardRst = 1;
                    goto exit;
                }
                break;
            case ESP_ERR_NVS_NOT_FOUND:     // If not initialized, then initialize
                if (initialize() != 0) {    // If something went wrong, then restart
                    goto exit;
                }
                break;
            default :
                goto exit;
        }
        gpio_set_level(GPIO_NUM_25, 0);     // Initialize/restore end
    }

    while(1) {
begin:
        // Wait for the callback to set the CONNECTED_BIT in the event group.
        xEventGroupWaitBits(wifiEventGroup, CONNECTED_BIT, false, true, portMAX_DELAY);

        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(PORT);           // Set the port of this protocol
        serv_addr.sin_addr.s_addr = inet_addr(IP[currNet]); // The IP address of the other machine
        sockfd = socket(AF_INET, SOCK_STREAM, 0);   // Setup the socket
        if (sockfd < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket: %d", errno);
            vTaskDelay(1000/portTICK_PERIOD_MS);
            goto exit;
        }
        ESP_LOGI(TAG, "... allocated socket");

        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno: %d", errno);
            ESP_LOGI(TAG, "Check that the server is running at the other end");
            invalidate();   // Invalidate / PIN Reset at a possible end of a connection
            close(sockfd);  // The connection may have failed because there is no server running
            vTaskDelay(5000/portTICK_PERIOD_MS);    // So wait a few seconds
            ESP_LOGI(TAG, "Trying again ...\n");    // And try to connect again
            goto begin;
        }
        ESP_LOGI(TAG, "... connected\n");

        bzero(recvBuf, sizeof(recvBuf));                // Zero the receive buffer
        r = read(sockfd, recvBuf, sizeof(recvBuf)-1);   // Receive the APDU command
        comAPDU = parseAPDU(recvBuf, r);                // Parse the APDU command

        if (comAPDU.INS == 0x00) {      // Nothing more to receive
            invalidate();   // Invalidate / PIN Reset at a possible end of a connection
            close(sockfd); 
            goto begin;
        }

#ifdef PRINTAPDU    // Print the parsed command APDU
        printf("CLA: %02X\tINS: %02X\tP1: %02X\t", comAPDU.CLA, comAPDU.INS, comAPDU.P1);
        printf("P2: %02X\tP1P2: %02X\tLc: %02X\tData: ", comAPDU.P2, comAPDU.P1P2, comAPDU.Lc);
        const uint8_t* tmp = comAPDU.data;
        while(*tmp)
            printf("%02X ", (unsigned int) *tmp++);
        printf("\nLe: %02X\tTotal: %d\n", comAPDU.Le, r);
        fflush(stdout);
#endif

#ifdef PROCEEDBTN   // The button has to be pressed before performing a security operation
        if ((comAPDU.CLA != 0x10) & (comAPDU.INS == 0x88 || comAPDU.INS == 0x2A)) { // Ignore for command chaining
            proceed = 0;    // Set the flag to 0
            int time = 0;   // Simple time counter

            while (proceed == 0 && time < 30) { // Wait until the button is pressed or time runs out
                gpio_set_level(GPIO_NUM_25, 1);     // Flash the LEDs to notify the user
                gpio_set_level(GPIO_NUM_26, 1);     // Flash the LEDs to notify the user
                vTaskDelay(250/portTICK_PERIOD_MS); // Wait for 250ms
                gpio_set_level(GPIO_NUM_25, 0);     // Flash the LEDs to notify the user
                gpio_set_level(GPIO_NUM_26, 0);     // Flash the LEDs to notify the user
                vTaskDelay(250/portTICK_PERIOD_MS); // Wait for another 250ms
                time++;     // 2 * 250ms * 30 = 15 seconds
            }
            gpio_set_level(GPIO_NUM_26, 1);         // Turn the WiFi status LED back on
            if (time == 30) {           // If time == 30 it means that the time ran out
                output.data[0] = 0x69;  // Set the output to SW_AUTHENTICATION_BLOCKED
                output.data[1]= 0x83;   // SW_AUTHENTICATION_BLOCKED = 0x6983
                output.length = 2;      // Set the length of the output to 2 bytes
                goto writeOutput;       // And bypass the processing of this APDU command
            }
        }
#endif

        gpio_set_level(GPIO_NUM_25, 1);     // Start processing a command

#ifdef TIMING                               // If TIMING is defined, print the duration of each operation
        uint32_t startTime, endTime;
        startTime = system_get_time();      // Yes, it is deprecated, but it's fine for this job
#endif

        process(comAPDU, &output);          // Perform the appropriate operation

#ifdef TIMING
        endTime = system_get_time();
        uint32_t duration = endTime - startTime;
        uint32_t min = duration/60000000;
        uint32_t timeTmp = duration%60000000;
        uint32_t sec = timeTmp/1000000;
        timeTmp = timeTmp%1000000;
        uint32_t ms = timeTmp/1000;
        uint32_t us = timeTmp%1000;
        printf("\t\t(mm:ss:mls:us)    /   Duration: %d us\n", duration);
        printf("\t\t %02d:%02d:%03d:%03d\n", min, sec, ms, us);
        if (comAPDU.INS == 0x84 && comAPDU.CLA == 0x00) {   // Get Challenge: 0x84
            timeCount++;            // Keep track of the number of times a specific operation has been performed
            totalTime += duration;  // Keep the total time taken in order to calculate an average
            printf("Number of operations: %d\t\tTotal time: %lld us\t\t", timeCount, totalTime);
            printf("Average time: %lld\n", totalTime/timeCount);
        }
        fflush(stdout);

        vTaskDelay(1000/portTICK_PERIOD_MS);    // To avoid watchdog starvation on repeated operations
#endif
        gpio_set_level(GPIO_NUM_25, 0);     // End of command processing

#ifdef PRINTAPDU    // Print the response APDU and it's length
        printf("Output Data: ");
        const uint8_t* tmp2 = output.data;
        while(*tmp2)
            printf("%02X ", (unsigned int) *tmp2++);
        printf("\nLength: %d\n", output.length);
        fflush(stdout);
#endif

#ifdef PROCEEDBTN
writeOutput:    // Label to jump if pressing the button is required and it didn't happen
#endif

        if (write(sockfd, output.data, output.length) < 0) {    // Write the response
            ESP_LOGE(TAG, "... socket send failed");
            vTaskDelay(1000/portTICK_PERIOD_MS);
            close(sockfd);
            goto exit;
        }
        ESP_LOGI(TAG, "... socket send success\n");
        close(sockfd);
    }

exit:   // Restart the system, in a controlled manner
    for (int countdown = 3; countdown > 0; countdown--) {
        ESP_LOGI(TAG, "Restart in: %d... ", countdown);
        vTaskDelay(1000/portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "Starting again");
    unmountFS();
    esp_restart();
}

static void checkReset(void *pvParameters) {
    while(1) {      // Check periodically (by polling) if reset button has been pressed
        if (hardRst == 1) {     // If it was pressed, erase "initialized" from NVS
            nvs_handle nvsHandle;
            if (nvs_open("storage", NVS_READWRITE, &nvsHandle) == ESP_OK) {
                if (nvs_erase_key(nvsHandle, "initialized") == ESP_OK) {
                    unmountFS();    // This will result in a re-initialization
                    esp_restart();  // When the ESP32 restarts
                }
            }
        }
        vTaskDelay(4000/portTICK_PERIOD_MS);
    }
}

static void wifiStatus(void *pvParameters) {
    uint8_t toggle = 0;     // Toggle for WiFi status LED
    while(1) {  // Flash while it is still looking for a known network
        if (connected == 0) {
            toggle ^= 1;    // Because using XOR to toggle 0 and 1 is cool
            gpio_set_level(GPIO_NUM_26, toggle);
        }
        vTaskDelay(500/portTICK_PERIOD_MS);
    }
}

void app_main() {
    initGPIO();     // Initialize the Input/Output pins
    initNVS();      // Initialize the Non-Volatile Storage
    if (!mountFS()) {   // Mount the FileSystem
        exit(0);
    }
    initWiFi();     // Initialize the WiFi
    xTaskCreate(&taskConnect, "taskConnect", 8192, NULL, 5, NULL);
    xTaskCreate(&checkReset, "checkReset", 2048, NULL, 5, NULL);
    xTaskCreate(&wifiStatus, "wifiStatus", 512, NULL, 5, NULL);
}