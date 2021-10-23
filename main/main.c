// Copyright 2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "bt_app_core.h"
#include "bt_app_av.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "driver/i2s.h"

#include <sys/param.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define ESP_PORT		   	CONFIG_ESP_PORT
#define ESP_WIFI_SSID      	CONFIG_ESP_WIFI_SSID
#define ESP_WIFI_PASS      	CONFIG_ESP_WIFI_PASS
#define ESP_MAXIMUM_RETRY  	3
#define	MAX_UDP_PACKAGE		1450


/* DEBUG */
//#define DEBUG
static const char 			*TAG = "UDP_SERVER";


/* FreeRTOS event group to signal when we are connected*/
EventGroupHandle_t s_wifi_event_group;
int s_retry_num = 0;
RingbufHandle_t s_ringbuf_wifi = NULL;
extern RingbufHandle_t s_ringbuf_i2s;
char rx_buffer[1500];
const StaticTask_t *task_tra_udp;
int to_send, sent, sending;
extern void bt_i2s_task_start_up(void);

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

struct sockaddr_in local_addr;
struct sockaddr_in origin_addr;
int8_t		broadcast_msg = 0;
uint32_t	broadcast_addr;
int sock;



/* event for handler "bt_av_hdl_stack_up */
enum {
    BT_APP_EVT_STACK_UP = 0,
};

/* handler for bluetooth stack enabled events */

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

/* handler for wifi stack enabled events */

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
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
        broadcast_addr = event->ip_info.ip.addr | 0xFF000000;
        ESP_LOGI(TAG, "broadcast ip: %s:", inet_ntoa(broadcast_addr));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_LOGI(TAG, "cfg.dynamic_rx_buf_num = %d", cfg.dynamic_rx_buf_num);
    ESP_LOGI(TAG, "cfg.dynamic_tx_buf_num = %d", cfg.dynamic_tx_buf_num);
    ESP_LOGI(TAG, "cfg.wifi_task_core_id = %d", cfg.wifi_task_core_id);


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

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

static void udp_tra_task(void *arg)
{


	size_t item_size;
	int err, pck_count = 0;

	while (1) {

		uint8_t *data = (uint8_t *)xRingbufferReceive(s_ringbuf_wifi, &item_size, (portTickType)portMAX_DELAY);
		//ESP_LOGI(TAG, "WF <- Len: %d  3st Buff: %d, %d, %d", item_size, (int) *data, (int) *(data +1), (int) *(data +2));
        if (item_size != 0){
    		if (broadcast_msg) {
    			origin_addr.sin_addr.s_addr = broadcast_addr;
    			origin_addr.sin_port = htons(ESP_PORT);
    			broadcast_msg = 0;
    		}
    		to_send = item_size;
			sent = 0;
			while(sent < item_size){
				if(to_send > MAX_UDP_PACKAGE){
					sending = MAX_UDP_PACKAGE;
				} else {
					sending = to_send;
				}
				err = sendto(sock, (data + sent), sending, 0, (struct sockaddr *)&origin_addr, sizeof(origin_addr));
				//ESP_LOGI(TAG, " T-> Len: %d  3st Buff: %d, %d, %d", sending, (int) *(data + sent), (int) *(data + sent + 1), (int) *(data + sent + 2));
				++pck_count;
				if (err < 0) {
					ESP_LOGE(TAG, "Error occurred during sending: errno %d, pckt: %d", errno, pck_count);
					break;
				}
				sent += sending;
				to_send -= sending;
			}
			vRingbufferReturnItem(s_ringbuf_wifi,(void *)data);
		}
	}

	vTaskDelete(NULL);
}

static void udp_server_task(void *arg)
{
    //char addr_str[128];
    char *rx_buff = &rx_buffer[0];
    int addr_family = AF_INET;
    int ip_protocol = 0;

    BaseType_t done;
    TaskHandle_t xSendUDP = NULL;
    socklen_t socklen = sizeof(origin_addr);
    int len;

    s_ringbuf_wifi = xRingbufferCreate(8 * 1024, RINGBUF_TYPE_BYTEBUF);
	if(s_ringbuf_wifi == NULL){
		ESP_LOGE(TAG, "Cannot create WiFi ringbuffer!!!");
	}

    while (1) {

    	local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    	origin_addr.sin_addr.s_addr = broadcast_addr;
    	local_addr.sin_family = AF_INET;
    	origin_addr.sin_family = AF_INET;
    	local_addr.sin_port = htons(ESP_PORT);
    	origin_addr.sin_port = htons(ESP_PORT);
		ip_protocol = IPPROTO_IP;

		sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
		if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket.");
            break;
        }
//		int bc = 1;
//		if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc)) < 0) {
//		    ESP_LOGE(TAG, "Failed to set sock options: errno %d", errno);
//		    closesocket(sock);
//		    break;
//		}

        ESP_LOGI(TAG, "Socket created");

        int err = bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind.");
        }
        ESP_LOGI(TAG, "Socket bound, port %d", ESP_PORT);

        xTaskCreatePinnedToCore(udp_tra_task, "udp_trans", 2 * 1024, NULL, configMAX_PRIORITIES - 3, &xSendUDP, 1);

        while (1) {

            //ESP_LOGI(TAG, "Waiting for data ...");
            len = recvfrom(sock, rx_buff, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&origin_addr, &socklen);
            //ESP_LOGI(TAG, "R<- Len: %d  3st Buff: %d, %d, %d", len, (int) *rx_buff, (int) *(rx_buff +1), (int) *(rx_buff +2));
            //ESP_LOGI(TAG, "Received on port %d, --> %s", ESP_PORT, rx_buffer);
//            if (++s_pkt_cnt % 100 == 0) {
//    		        ESP_LOGI(BT_AV_TAG, ".");
//    		    }
//            // Error occurred during receiving
//            if (len < 0) {
//                ESP_LOGE(TAG, "recvfrom failed.");
//                break;
//            }
//            // Data received
//            else {
//                // Get the sender's ip address as string
//                inet_ntoa_r(((struct sockaddr_in *)&origin_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
//                //rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string...
#if (A2DP_SINK_OUTPUT_EXTERNAL_I2S == TRUE)

                done = xRingbufferSend(s_ringbuf_i2s, (void *) rx_buff, len, (portTickType)portMAX_DELAY);
        		//ESP_LOGI(TAG, "s_ringbuf_i2s done: %u, len: %d", done, len);
//                if(!done){
//                	ESP_LOGI(TAG, "s_ringbuf_i2s overload");
//				}
#endif
            }
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }

    vTaskDelete(NULL);
}

void app_main(void)
{
	TaskHandle_t xReceiveUDP = NULL;
    //Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Starts WiFi configuration

    wifi_init_sta();

    xTaskCreatePinnedToCore(udp_server_task, "udp_server", 2 * 1024 , NULL, configMAX_PRIORITIES - 2, &xReceiveUDP, 1);

    // Starts I2S configuration

    i2s_config_t i2s_config = {

    	.mode = I2S_MODE_MASTER | I2S_MODE_TX,                                  // Only TX
        .sample_rate = 44100,
        .bits_per_sample = 16,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,                           //2-channels
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .dma_buf_count = 6,
        .dma_buf_len = 60,
        .intr_alloc_flags = 0,                                                  //Default interrupt priority
        .tx_desc_auto_clear = true                                              //Auto clear tx descriptor on underflow
    };


    i2s_driver_install(0, &i2s_config, 0, NULL);
    i2s_pin_config_t pin_config = {
        .bck_io_num = CONFIG_I2S_BCK_PIN,
        .ws_io_num = CONFIG_I2S_LRCK_PIN,
        .data_out_num = CONFIG_I2S_DATA_PIN,
        .data_in_num = -1                                                       //Not used
    };

    i2s_set_pin(0, &pin_config);

    bt_i2s_task_start_up();

    // Starts BlueTooth configuration

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(err));
        return;
    }

    if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(err));
        return;
    }

    if ((err = esp_bluedroid_init()) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(err));
        return;
    }

    if ((err = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(err));
        return;
    }

    /* create application task */
    bt_app_task_start_up();

    /* Bluetooth device name, connection mode and profile set up */
    bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);

#if (CONFIG_BT_SSP_ENABLED == true)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    /*
     * Set default parameters for Legacy Pairing
     * Use fixed pin code
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code;
    pin_code[0] = '1';
    pin_code[1] = '2';
    pin_code[2] = '3';
    pin_code[3] = '4';
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    ESP_LOGW(TAG, "esp_get_free_heap_size(): %d", esp_get_free_heap_size());
}

void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
            esp_log_buffer_hex(BT_AV_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(BT_AV_TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
        break;

    default: {
        ESP_LOGI(BT_AV_TAG, "event: %d", event);
        break;
    }
    }
    return;
}
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param) {

    ESP_LOGD(BT_AV_TAG, "%s evt %d", __func__, event);
    switch (event) {
    case BT_APP_EVT_STACK_UP: {
        /* set up device name */
        char *dev_name = CONFIG_CENTRAL_NAME;
        esp_bt_dev_set_device_name(dev_name);

        esp_bt_gap_register_callback(bt_app_gap_cb);

        /* initialize AVRCP controller */
        esp_avrc_ct_init();
        esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
        /* initialize AVRCP target */
        assert (esp_avrc_tg_init() == ESP_OK);
        esp_avrc_tg_register_callback(bt_app_rc_tg_cb);

        esp_avrc_rn_evt_cap_mask_t evt_set = {0};
        esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
        assert(esp_avrc_tg_set_rn_evt_cap(&evt_set) == ESP_OK);

        /* initialize A2DP sink */
        esp_a2d_register_callback(&bt_app_a2d_cb);
        esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);
        esp_a2d_sink_init();

        /* set discoverable and connectable mode, wait to be connected */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
        break;
    }
}
