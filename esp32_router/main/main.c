

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "lwip/icmp.h"
#include "esp_wifi.h"

#define STA_SSID "VM7312921"
#define STA_PASS "fhzfuKd6brgucct5"
#define AP_SSID "ESP32_Bridge_AP"
#define AP_PASS "ESP32Password"

// Define the NAT table
#define NAT_TABLE_SIZE 100
typedef struct {
    uint32_t src_ip;       // Source IP of the original packet
    uint32_t src_port;     // Source port of the original packet
    uint32_t dst_ip;       // Destination IP of the original packet
    uint32_t dst_port;     // Destination port of the original packet
} nat_entry_t;

nat_entry_t nat_table[NAT_TABLE_SIZE];  // Basic NAT table
uint8_t nat_table_idx = 0;              // Index to track NAT table entries

// Wi-Fi Configuration for Station (STA) mode
esp_wifi_config_t sta_config = {
    .sta = {
        .ssid = STA_SSID,
        .password = STA_PASS,
    },
};

// Wi-Fi Configuration for Access Point (AP) mode
esp_wifi_config_t ap_config = {
    .ap = {
        .ssid = AP_SSID,
        .password = AP_PASS,
        .ssid_len = strlen(AP_SSID),
        .channel = 1,
        .authmode = WIFI_AUTH_WPA2_PSK,
        .max_connection = 4,
        .beacon_interval = 100,
    },
};

// Function to initialize Wi-Fi in Station (STA) mode
esp_err_t wifi_init_sta() {
    ESP_LOGI("wifi", "Connecting to home network %s...", STA_SSID);

    esp_wifi_set_mode(WIFI_MODE_STA); // Set to Station mode
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_start();

    return ESP_OK;
}

// Function to initialize Wi-Fi in Access Point (AP) mode
esp_err_t wifi_init_ap() {
    ESP_LOGI("wifi", "Starting AP: %s...", AP_SSID);

    esp_wifi_set_mode(WIFI_MODE_AP); // Set to Access Point mode
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    return ESP_OK;
}

// Function to initialize Wi-Fi in Station + Access Point (STA + AP) mode
esp_err_t wifi_init_sta_ap() {
    ESP_LOGI("wifi", "Starting STA + AP mode...");

    esp_wifi_set_mode(WIFI_MODE_STA_AP); // Set to Station + Access Point mode
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    return ESP_OK;
}

// Function to calculate IP checksum (use for IP header)
unsigned short ip_checksum(unsigned short *buffer, int len) {
    unsigned long sum = 0;
    unsigned short *ptr = buffer;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len) {
        sum += *((unsigned char *) ptr);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

// Simple function to modify source and destination IP address (DNAT/SNAT)
void modify_packet_ip(char *packet, int len, bool is_incoming) {
    struct ip_hdr *ip_header = (struct ip_hdr*) packet;
    
    // Source NAT: Change source IP if outgoing from STA (sent to AP)
    if (!is_incoming) {
        ip_header->src.addr = htonl(0xC0A80001);  // Change source IP to ESP32 IP (e.g., 192.168.0.1)
    }
    
    // Destination NAT: Change destination IP if incoming from AP (sent to STA)
    if (is_incoming) {
        ip_header->dst.addr = htonl(0xC0A80001);  // Change destination IP to ESP32 IP
    }

    ip_header->chksum = 0;  // Reset checksum before recalculating
    ip_header->chksum = ip_checksum((unsigned short*)ip_header, ip_header->len * 4);  // Recalculate checksum
}

// Function to forward packets from the AP interface to the STA interface
void forward_packet_from_ap_to_sta(char *packet, int len) {
    // Modify the packet for DNAT (destination NAT)
    modify_packet_ip(packet, len, true);
    
    // Forward the packet from AP to STA
    esp_wifi_80211_tx(WIFI_IF_STA, (void*)packet, len, true);
}

// Function to forward packets from the STA interface to the AP interface
void forward_packet_from_sta_to_ap(char *packet, int len) {
    // Modify the packet for SNAT (source NAT)
    modify_packet_ip(packet, len, false);
    
    // Forward the packet from STA to AP
    esp_wifi_80211_tx(WIFI_IF_AP, (void*)packet, len, true);
}

// Simple packet capture and forwarding loop
void capture_and_forward_packets() {
    char packet_buffer[1500]; // Adjust buffer size as needed
    int len;

    while (1) {
        // Capture packets from AP interface
        len = esp_wifi_80211_rx(WIFI_IF_AP, packet_buffer, sizeof(packet_buffer));
        if (len > 0) {
            // Forward the packet from AP to STA
            forward_packet_from_ap_to_sta(packet_buffer, len);
        }

        // Capture packets from STA interface
        len = esp_wifi_80211_rx(WIFI_IF_STA, packet_buffer, sizeof(packet_buffer));
        if (len > 0) {
            // Forward the packet from STA to AP
            forward_packet_from_sta_to_ap(packet_buffer, len);
        }

        vTaskDelay(10); // Delay to prevent busy-waiting, adjust as needed
    }
}

// Main application entry point
void app_main() {
    // Initialize NVS (needed for Wi-Fi functionality)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Set the desired Wi-Fi mode
    esp_wifi_set_mode(WIFI_MODE_STA);  // Change to WIFI_MODE_AP or WIFI_MODE_STA_AP as needed

    // Initialize Wi-Fi based on selected mode
    if (WIFI_MODE == WIFI_MODE_STA) {
        ESP_ERROR_CHECK(wifi_init_sta());
    } else if (WIFI_MODE == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(wifi_init_ap());
    } else if (WIFI_MODE == WIFI_MODE_STA_AP) {
        ESP_ERROR_CHECK(wifi_init_sta_ap());
    }

    ESP_LOGI("wifi", "ESP32 Router Started");

    // Start packet capture and forwarding in the background
    capture_and_forward_packets();
}
