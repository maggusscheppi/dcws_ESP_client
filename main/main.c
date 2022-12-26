/* PPPoS Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "mqtt_client.h"
#include "esp_modem.h"
#include "esp_modem_netif.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "driver/gpio.h"
#include "sim800.h"
#include "nmea_parser.h"

#define TIME_ZONE (+0)   //UTC
#define YEAR_BASE (2000) //date in GPS starts from 2000



static const char *TAG = "dcws_gsm";
static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT = BIT0;
static const int STOP_BIT = BIT1;
static char macAddr[18];


#define MODEM_PWKEY 4
#define MODEM_RST 5
#define MODEM_POWER_ON 23


#define HIGH 1
#define LOW  

#define NO_DATA_TIMEOUT_SEC 10
#define CONFIG_WEBSOCKET_URI "wss://schepplications.de:8091"

static TimerHandle_t shutdown_signal_timer;
static SemaphoreHandle_t shutdown_sema;

static void shutdown_signaler(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "No data received for %d seconds, signaling shutdown", NO_DATA_TIMEOUT_SEC);
    xSemaphoreGive(shutdown_sema);
}


/*
* modem specific elements
*/
modem_dte_t *dte = NULL;
modem_dce_t *dce = NULL;


static void modem_down()
{
	ESP_LOGI(TAG, "power down modem");
    /* Power down module */
    ESP_ERROR_CHECK(dce->power_down(dce));
    ESP_LOGI(TAG, "Power down");
    ESP_ERROR_CHECK(dce->deinit(dce));
    ESP_ERROR_CHECK(dte->deinit(dte));
}


/*
* websocket stuff
*/
esp_websocket_client_handle_t client;

static void websocket_stop(void)
{	
	ESP_LOGI(TAG, "stopping websocket");
    xSemaphoreTake(shutdown_sema, portMAX_DELAY);
    esp_websocket_client_stop(client);
    ESP_LOGI(TAG, "Websocket Stopped");
    esp_websocket_client_destroy(client);
}


static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
		// websocket_stop();
		// modem_down();
		// vTaskDelay(500 / portTICK_RATE_MS);
		ESP_LOGI(TAG, "restarting ESP");
		esp_restart();
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
        ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
        ESP_LOGW(TAG, "Received=%.*s", data->data_len, (char *)data->data_ptr);
        ESP_LOGW(TAG, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n", data->payload_len, data->data_len, data->payload_offset);

        xTimerReset(shutdown_signal_timer, portMAX_DELAY);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    }
}


static void websocket_start()
{
    esp_websocket_client_config_t websocket_cfg = {};

    shutdown_signal_timer = xTimerCreate("Websocket shutdown timer", NO_DATA_TIMEOUT_SEC * 1000 / portTICK_PERIOD_MS,
                                         pdFALSE, NULL, shutdown_signaler);
    shutdown_sema = xSemaphoreCreateBinary();


    websocket_cfg.uri = CONFIG_WEBSOCKET_URI;

    ESP_LOGI(TAG, "Connecting to %s...", websocket_cfg.uri);

    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    esp_websocket_client_start(client);
    xTimerStart(shutdown_signal_timer, portMAX_DELAY);
}



static void websocket_send(char *data, int iLength)
{
	ESP_LOGI(TAG, "Sending %d chars; %s", iLength, data);
	/*
    char data[100];
    int i = 0;
	while (i < 1) {
		if (esp_websocket_client_is_connected(client)) {
			int len = sprintf(data, "2020-12-06T22:00:00.000Z;%s;50.57851590;8.82051920;0.0000;0.00;0", macAddr);
			ESP_LOGI(TAG, "Sending %s", data);
			esp_websocket_client_send(client, data, len, portMAX_DELAY);
			i++;
		}
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
	*/
	if (esp_websocket_client_is_connected(client)) {
		esp_websocket_client_send(client, data, iLength, portMAX_DELAY);
	}
}

/*
* a send buffer
*/
char szData[100];

/**
 * @brief GPS Event Handler
 *
 * @param event_handler_arg handler specific arguments
 * @param event_base event base, here is fixed to ESP_NMEA_EVENT
 * @param event_id event id
 * @param event_data event specific arguments
 */
static void gps_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    gps_t *gps = NULL;
    switch (event_id) {
    case GPS_UPDATE:
        gps = (gps_t *)event_data;
        /* print information parsed from GPS statements */
        ESP_LOGI(TAG, "%d-%02d-%02d %02d:%02d:%02d => \r\n"
                 "\t\t\t\t\t\tlatitude   = %.08f°N\r\n"
                 "\t\t\t\t\t\tlongitude  = %.08f°E\r\n"
                 "\t\t\t\t\t\taltitude   = %.02fm\r\n"
                 "\t\t\t\t\t\tspeed      = %.0fkm/h\r\n"
                 "\t\t\t\t\t\tdeg        = %.0f°\r\n"
                 "\t\t\t\t\t\tSat(view)  = %02d\r\n"
                 "\t\t\t\t\t\tSat(use)   = %02d",
                 gps->date.year + YEAR_BASE, gps->date.month, gps->date.day,
                 gps->tim.hour, gps->tim.minute, gps->tim.second,
                 gps->latitude, gps->longitude, gps->altitude, (gps->speed*3.6),
				 gps->cog, gps->sats_in_view, gps->sats_in_use);
		if (gps->fix != GPS_FIX_INVALID) {
			ESP_LOGI(TAG, "we got a GPS-Fix!");
			/*
			ESP_LOGI(TAG, "%d-%02d-%02dT%02d:%02d:%02d.000Z;%s;%.08f;%.08f;%.02f;%.0f;%0f",
				gps->date.year + YEAR_BASE, gps->date.month, gps->date.day,
                gps->tim.hour, gps->tim.minute, gps->tim.second, macAddr,
                gps->latitude, gps->longitude, gps->altitude, 
				(gps->speed*3.6), gps->cog);
			*/
			int len = sprintf(szData, "%d-%02d-%02dT%02d:%02d:%02d.000Z;%s;%.08f;%.08f;%.02f;%.0f;%.0f",
				gps->date.year + YEAR_BASE, gps->date.month, gps->date.day,
                gps->tim.hour, gps->tim.minute, gps->tim.second, macAddr,
                gps->latitude, gps->longitude, gps->altitude, 
				(gps->speed*3.6), gps->cog);
			websocket_send(szData, len);
		}
        break;
    case GPS_UNKNOWN:
        /* print unknown statements */
        ESP_LOGW(TAG, "Unknown statement:%s", (char *)event_data);
        break;
    default:
        break;
    }
}




static void modem_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ESP_MODEM_EVENT_PPP_START:
        ESP_LOGI(TAG, "Modem PPP Started");
        break;
    case ESP_MODEM_EVENT_PPP_STOP:
        ESP_LOGI(TAG, "Modem PPP Stopped");
        xEventGroupSetBits(event_group, STOP_BIT);
        break;
    case ESP_MODEM_EVENT_UNKNOWN:
        ESP_LOGW(TAG, "Unknow line received: %s", (char *)event_data);
        break;
    default:
        break;
    }
}


static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "PPP state changed event %d", event_id);
    if (event_id == NETIF_PPP_ERRORUSER) {
        /* User interrupted event from esp-netif */
        esp_netif_t *netif = event_data;
        ESP_LOGI(TAG, "User interrupted event from netif:%p", netif);
    }
}


static void on_ip_event(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "IP event! %d", event_id);
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        esp_netif_dns_info_t dns_info;

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_t *netif = event->esp_netif;

        ESP_LOGI(TAG, "Modem Connect to PPP Server");
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        esp_netif_get_dns_info(netif, 0, &dns_info);
        ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        esp_netif_get_dns_info(netif, 1, &dns_info);
        ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
		// get the mac address
		uint8_t derived_mac_addr[6] = {0};
		ESP_ERROR_CHECK(esp_read_mac(derived_mac_addr, ESP_MAC_ETH));
		int len = sprintf(macAddr, "%x:%x:%x:%x:%x:%x",             
			 derived_mac_addr[0], derived_mac_addr[1], derived_mac_addr[2],
             derived_mac_addr[3], derived_mac_addr[4], derived_mac_addr[5]);
		ESP_LOGI(TAG, "Ethernet MAC: %s", macAddr);
		
        xEventGroupSetBits(event_group, CONNECT_BIT);

        ESP_LOGI(TAG, "GOT ip event!!!");
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "Modem Disconnect from PPP Server");
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ESP_LOGI(TAG, "GOT IPv6 event!");

        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        ESP_LOGI(TAG, "Got IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}

	
static void modem_up()
{
    esp_netif_auth_type_t auth_type = NETIF_PPP_AUTHTYPE_PAP;
	
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    event_group = xEventGroupCreate();

    // Init netif object
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&cfg);
    assert(esp_netif);
		
    /* create dte object */
    esp_modem_dte_config_t config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte = esp_modem_dte_init(&config);
    /* Register event handler */
    ESP_ERROR_CHECK(esp_modem_set_event_handler(dte, modem_event_handler, ESP_EVENT_ANY_ID, NULL));
    /* create dce object */
    dce = sim800_init(dte);
	if (dce == NULL) {
        ESP_LOGE(TAG, "SIM 800 initialization failed");
        return;
    }

    ESP_ERROR_CHECK(dce->set_flow_ctrl(dce, MODEM_FLOW_CONTROL_NONE));
    ESP_ERROR_CHECK(dce->store_profile(dce));
    /* Print Module ID, Operator, IMEI, IMSI */
    ESP_LOGI(TAG, "Module: %s", dce->name);
    ESP_LOGI(TAG, "Operator: %s", dce->oper);
    ESP_LOGI(TAG, "IMEI: %s", dce->imei);
    ESP_LOGI(TAG, "IMSI: %s", dce->imsi);
    /* Get signal quality */
    uint32_t rssi = 0, ber = 0;
    ESP_ERROR_CHECK(dce->get_signal_quality(dce, &rssi, &ber));
    ESP_LOGI(TAG, "rssi: %d, ber: %d", rssi, ber);
    /* Get battery voltage */
    uint32_t voltage = 0, bcs = 0, bcl = 0;
    ESP_ERROR_CHECK(dce->get_battery_status(dce, &bcs, &bcl, &voltage));
    ESP_LOGI(TAG, "Battery voltage: %d mV", voltage);
    /* setup PPPoS network parameters */
    esp_netif_ppp_set_auth(esp_netif, auth_type, CONFIG_EXAMPLE_MODEM_PPP_AUTH_USERNAME, CONFIG_EXAMPLE_MODEM_PPP_AUTH_PASSWORD);
    void *modem_netif_adapter = esp_modem_netif_setup(dte);
    esp_modem_netif_set_default_handlers(modem_netif_adapter, esp_netif);
    /* attach the modem to the network interface */
    esp_netif_attach(esp_netif, modem_netif_adapter);
    /* Wait for IP address */
    xEventGroupWaitBits(event_group, CONNECT_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
}

	
void app_main(void)
{
	modem_up();
	
	websocket_start();
	
	vTaskDelay(3000 / portTICK_RATE_MS);
	
    /* NMEA parser configuration */
    nmea_parser_config_t nmea_config = NMEA_PARSER_CONFIG_DEFAULT();
    /* init NMEA parser library */
    nmea_parser_handle_t nmea_hdl = nmea_parser_init(&nmea_config);
    /* register event handler for NMEA parser library */
	ESP_LOGI(TAG, "Register NMEA event handler");
    nmea_parser_add_handler(nmea_hdl, gps_event_handler, NULL);		
	
	
	while (1==1) {
		// give CPU a chance to do things
		vTaskDelay(200 / portTICK_RATE_MS);
	}
}
