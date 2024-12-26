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
#include "lwip/ip4_addr.h"
#include "lwip/icmp.h"
#include "lwip/ip.h"
#include "lwip/raw.h"
#include "lwip/api.h"
#include "lwip/sys.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "esp_netif_net_stack.h"
#include <stdint.h>

// Declare global variables for Wi-Fi interfaces
esp_netif_t *sta_handle = NULL;  // Station interface handle
esp_netif_t *ap_handle = NULL;   // Access Point interface handle

ip_addr_t translated_ip;
uint16_t translated_port;
bool is_snat = false;
bool is_dnat = true;

// Wi-Fi Configuration for Station (STA) mode
#define STA_SSID "VM7312921"
#define STA_PASS "fhzfuKd6brgucct5"
#define AP_SSID "ESP32_Bridge_AP"
#define AP_PASS "ESP32Password"


// Define the NAT table size
#define NAT_TABLE_SIZE 100

// Define the structure first, as other code will depend on it
typedef struct {
    ip_addr_t src_ip;        // Source IP of the original packet
    uint32_t src_port;       // Source port of the original packet
    ip_addr_t dest_ip;       // Destination IP of the original packet
    uint32_t dest_port;      // Destination port of the original packet
    ip_addr_t translated_ip; // Translated IP for SNAT or DNAT
    uint32_t translated_port;// Translated port (if port translation is used)
    uint8_t is_snat;         // Flag to identify if the entry is for SNAT
    uint8_t is_dnat;         // Flag to identify if the entry is for DNAT
} nat_entry_t;

// Declare global variables
nat_entry_t nat_table[NAT_TABLE_SIZE];  // NAT table
uint8_t nat_table_idx = 0;              // Index to track NAT table entries
uint8_t nat_enabled = 0;                // Flag to enable/disable NAT functionality

// Declare external variables
extern struct netif *inp;               // Network interface to send the packet

// Logging tag
static const char *TAG = "wifi_event_handler";

// Example Wi-Fi configuration (ensure STA_SSID and STA_PASS are defined)
wifi_config_t sta_config = {
    .sta = {
        .ssid = STA_SSID,
        .password = STA_PASS,
    },
};

wifi_config_t ap_config = {
    .ap = {
        .ssid = AP_SSID,
        .password = AP_PASS,
        .ssid_len = strlen(AP_SSID),
        .channel = 11,
        .authmode = WIFI_AUTH_WPA2_PSK,
        .max_connection = 4,
        .beacon_interval = 100,
        .ssid_hidden = 0, // Ensure SSID is visible
    },
};


bool is_sta_network(uint32_t dest_ip) {
    ip4_addr_t sta_ip;
    ip4_addr_t sta_netmask;

    // Get STA IP and netmask
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta_handle, &ip_info) == ESP_OK) {
        sta_ip.addr = ip_info.ip.addr;
        sta_netmask.addr = ip_info.netmask.addr;
    } else {
        ESP_LOGE(TAG, "Failed to get STA IP info");
        return false;
    }

    // Check if destination IP belongs to the STA network
    if ((dest_ip & sta_netmask.addr) == (sta_ip.addr & sta_netmask.addr)) {
        return true;
    }
    return false;
}


esp_netif_t* get_netif(esp_netif_t *netif_handle) {
    const char *ifkey = esp_netif_get_ifkey(netif_handle);  // Retrieve the interface key (string)
    return esp_netif_get_handle_from_ifkey(ifkey);  // Return the esp_netif_t* handle
}





struct netif* esp_netif_get_netif_impl_mine(esp_netif_t *esp_netif) {
    if (!esp_netif) {
        ESP_LOGE(TAG, "ESP-Netif handle is NULL!");
        return NULL;
    }

    // Get the interface key to identify the lwIP netif
    const char *if_key = esp_netif_get_ifkey(esp_netif);
    if (!if_key) {
        ESP_LOGE(TAG, "Failed to get ifkey for ESP-Netif handle");
        return NULL;
    }

    // Iterate through lwIP netif list to find the matching interface
    struct netif *netif = netif_list;
    while (netif) {
        if (strcmp(netif->name, if_key) == 0) {
            return netif;  // Matching lwIP netif found
        }
        netif = netif->next;  // Move to the next netif
    }

    ESP_LOGE(TAG, "No matching lwIP netif found for ifkey: %s", if_key);
    return NULL;
}



struct netif* get_lwip_netif_impl_mine(esp_netif_t *esp_netif) {
    // Retrieve the lwIP network interface
    struct netif *lwip_netif = esp_netif_get_netif_impl_mine(esp_netif);

    // Log and return
    if (lwip_netif) {
        ESP_LOGI(TAG, "IP Address: %s", ip4addr_ntoa(&lwip_netif->ip_addr));
    } else {
        ESP_LOGE(TAG, "lwIP netif implementation not found!");
    }
    return lwip_netif;
}

// Function to dynamically track and add a new NAT entry
void track_connection(ip_addr_t src_ip, uint32_t src_port, ip_addr_t dest_ip, uint32_t dest_port, uint8_t is_snat, uint8_t is_dnat) {
    if (nat_table_idx < NAT_TABLE_SIZE) {
        // Add new entry when there is space
        nat_table[nat_table_idx].src_ip = src_ip;
        nat_table[nat_table_idx].src_port = src_port;
        nat_table[nat_table_idx].dest_ip = dest_ip;
        nat_table[nat_table_idx].dest_port = dest_port;
        // For dynamic translation, use the same IP/port initially (can be modified for SNAT/DNAT)
        nat_table[nat_table_idx].translated_ip = src_ip;
        nat_table[nat_table_idx].translated_port = src_port;
        nat_table[nat_table_idx].is_snat = is_snat;
        nat_table[nat_table_idx].is_dnat = is_dnat;
        nat_table_idx++;
        ESP_LOGI("NAT", "Connection tracked and NAT entry created.");
    } else {
        // If the table is full, evict the oldest entry (simple eviction strategy)
        ESP_LOGW("NAT", "NAT table full, evicting the oldest entry.");
        
        // Shift all entries to make space for the new one
        for (int i = 1; i < NAT_TABLE_SIZE; i++) {
            nat_table[i - 1] = nat_table[i];
        }

        // Insert the new entry at the end of the table
        nat_table[NAT_TABLE_SIZE - 1].src_ip = src_ip;
        nat_table[NAT_TABLE_SIZE - 1].src_port = src_port;
        nat_table[NAT_TABLE_SIZE - 1].dest_ip = dest_ip;
        nat_table[NAT_TABLE_SIZE - 1].dest_port = dest_port;
        nat_table[NAT_TABLE_SIZE - 1].translated_ip = src_ip;
        nat_table[NAT_TABLE_SIZE - 1].translated_port = src_port;
        nat_table[NAT_TABLE_SIZE - 1].is_snat = is_snat;
        nat_table[NAT_TABLE_SIZE - 1].is_dnat = is_dnat;

        ESP_LOGI("NAT", "Oldest NAT entry evicted and new entry added.");
    }
}

// Function to add a NAT entry
void add_nat_entry(ip_addr_t src_ip, uint32_t src_port, ip_addr_t dest_ip, uint32_t dest_port, ip_addr_t translated_ip, uint32_t translated_port, uint8_t is_snat, uint8_t is_dnat) {
    if (nat_table_idx < NAT_TABLE_SIZE) {
        nat_table[nat_table_idx].src_ip = src_ip;
        nat_table[nat_table_idx].src_port = src_port;
        nat_table[nat_table_idx].dest_ip = dest_ip;
        nat_table[nat_table_idx].dest_port = dest_port;
        nat_table[nat_table_idx].translated_ip = translated_ip;
        nat_table[nat_table_idx].translated_port = translated_port;
        nat_table[nat_table_idx].is_snat = is_snat;
        nat_table[nat_table_idx].is_dnat = is_dnat;
        nat_table_idx++;
    } else {
        ESP_LOGW("NAT", "NAT table full, unable to add entry.");
    }
}


void check_lwip_netif() {
    struct netif *lwip_netif_sta = esp_netif_get_netif_impl_mine(sta_handle);
    if (lwip_netif_sta) {
        ESP_LOGI(TAG, "STA lwIP netif is available");
    }

    struct netif *lwip_netif_ap = esp_netif_get_netif_impl_mine(ap_handle);
    if (lwip_netif_ap) {
        ESP_LOGI(TAG, "AP lwIP netif is available");
    }
}

void packet_interception(struct pbuf *p, struct netif *inp) {
    // Ensure the packet is large enough to contain an IP header
    if (p->len < sizeof(struct ip_hdr)) {
        ESP_LOGW("Packet", "Packet too small to contain IP header");
        return;
    }

    // Cast the payload to the IP header structure
    struct ip_hdr *ip_header = (struct ip_hdr *)p->payload;

    // Check if lwIP network interfaces are available
    check_lwip_netif();

    // Handle network interface assignment based on the destination IP
    if (is_sta_network(ip_header->dest.addr)) {
    inp = get_lwip_netif_impl_mine(sta_handle);  // Use the helper function for STA
    } else {
    inp = get_lwip_netif_impl_mine(ap_handle);  // Use the helper function for AP
    }

    // If NAT is enabled, apply NAT translation
    if (nat_enabled) {
        for (int i = 0; i < NAT_TABLE_SIZE; i++) {
            // Check if source IP matches for SNAT
            ip4_addr_t tmp_ip;
            // Initialize the IP address 192.168.0.1
            IP4_ADDR(&tmp_ip, 192, 168, 0, 1);

            // Check if the current nat_table entry matches the desired IP
            if (nat_table[i].is_snat && nat_table[i].src_ip.u_addr.ip4.addr == tmp_ip.addr) {
                // Apply source NAT translation
                ip_header->src.addr = PP_HTONL(LWIP_MAKEU32(192, 168, 1, 1));  // Example address (replace with actual)
                ESP_LOGI("Packet", "SNAT applied: %s -> %s", 
		ip4addr_ntoa((const ip4_addr_t *)&nat_table[i].src_ip), 
	        ip4addr_ntoa((const ip4_addr_t *)&nat_table[i].translated_ip));
                break;
            }

            // Check if destination IP matches for DNAT
                // Apply destination NAT translation
		// Initialize the IP address 192.168.0.1
		IP4_ADDR(&tmp_ip, 192, 168, 0, 1);

		// Check if the NAT table entry is DNAT and the destination IP matches 192.168.0.1
		if (nat_table[i].is_dnat && ip_header->dest.addr == tmp_ip.addr) {
			ip_header->dest.addr = PP_HTONL(LWIP_MAKEU32(192, 168, 1, 1));  // Example address (replace with actual)
			ESP_LOGI("Packet", "DNAT applied: %s -> %s", 
                        ip4addr_ntoa((const ip4_addr_t *)&nat_table[i].dest_ip), 
                        ip4addr_ntoa((const ip4_addr_t *)&nat_table[i].translated_ip));
                break;
            }
        }
    }

    // Handle Port Translation (PAT)
    for (int i = 0; i < NAT_TABLE_SIZE; i++) {
        // Check if source port needs translation for SNAT
        if (nat_table[i].is_snat && ip_header->src.addr == nat_table[i].src_ip.u_addr.ip4.addr) {
            // Modify the source port if needed
            if (nat_table[i].translated_port != 0) {
                ip_header->src.addr = nat_table[i].translated_ip.u_addr.ip4.addr;
                ESP_LOGI("Packet", "Port translation for SNAT: %" PRIu32 " -> %" PRIu32, 
                         nat_table[i].src_port, nat_table[i].translated_port);
            }
        }

        // Check if destination port needs translation for DNAT
        if (nat_table[i].is_dnat && ip_header->dest.addr == nat_table[i].dest_ip.u_addr.ip4.addr) {
            // Modify the destination port if needed
            if (nat_table[i].translated_port != 0) {
                ip_header->dest.addr = nat_table[i].translated_ip.u_addr.ip4.addr; 
                ESP_LOGI("Packet", "Port translation for DNAT: %" PRIu32 " -> %" PRIu32, 
                         nat_table[i].dest_port, nat_table[i].translated_port);
            }
        }
    }

    // Send the modified packet through the appropriate interface
    if (inp != NULL) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = 0; // Set to 0 for raw sockets
        dest_addr.sin_addr.s_addr = ip_header->dest.addr; // Send to the modified destination IP

        // Forward the packet via a raw socket
        int sock = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE("Packet", "Failed to create raw socket: %d", sock);
            return;  // Handle error appropriately
        }
        if (lwip_sendto(sock, p->payload, p->len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to send packet through raw socket");
    }
	lwip_close(sock);
    }
}


// Wi-Fi event handler to handle Wi-Fi events
void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi Station started");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Wi-Fi connected to the AP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "Wi-Fi disconnected from the AP");
            break;
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "Wi-Fi Access Point started");
            break;
        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "Wi-Fi Access Point stopped");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "A station has connected to the Access Point");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "A station has disconnected from the Access Point");
            break;
        default:
            ESP_LOGI(TAG, "Unknown Wi-Fi event: %ld", (long int)event_id);
            break;
    }
}


// Function to initialize raw socket
void raw_socket_init() {
    int sock = lwip_socket(AF_INET, SOCK_RAW, IP_PROTO_TCP);  // Create raw socket
    if (sock < 0) {
        ESP_LOGE("Raw Socket", "Failed to create raw socket: %d", sock);
        return;
    }

    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = 0;  // Bind to any port
    bind_addr.sin_addr.s_addr = INADDR_ANY;  // Bind to all IP addresses

    if (lwip_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE("Raw Socket", "Bind failed: %d", errno);
        lwip_close(sock);
        return;
    }

    ESP_LOGI("Raw Socket", "Raw socket initialized successfully.");
}

// Function to initialize Wi-Fi in Station (STA) mode
esp_err_t wifi_init_sta() {
	ESP_LOGI("wifi", "Connecting to home network %s...", STA_SSID);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_err_t err = esp_wifi_init(&cfg);
	if (err != ESP_OK) {
    	ESP_LOGE("wifi", "Error initializing Wi-Fi: %s", esp_err_to_name(err));
    	return err;
	}

	err = esp_wifi_set_mode(WIFI_MODE_STA); // Set to Station mode
	if (err != ESP_OK) {
    	ESP_LOGE("wifi", "Error setting Wi-Fi mode to STA: %s", esp_err_to_name(err));
    	return err;
	}

	err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
	if (err != ESP_OK) {
    	ESP_LOGE("wifi", "Error setting Wi-Fi configuration for STA: %s", esp_err_to_name(err));
    	return err;
	}

	err = esp_wifi_start();
	if (err != ESP_OK) {
    	ESP_LOGE("wifi", "Error starting Wi-Fi in STA mode: %s", esp_err_to_name(err));
    	return err;
	}

	ESP_LOGI("wifi", "STA Started on SSID: %s", STA_SSID);
	return ESP_OK;
}

// Function to initialize Wi-Fi in Access Point (AP) mode
esp_err_t wifi_init_ap() {
	ESP_LOGI("wifi", "Starting AP: %s...", AP_SSID);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_err_t err = esp_wifi_init(&cfg);
	if (err != ESP_OK) {
    	ESP_LOGE("wifi", "Error initializing Wi-Fi: %s", esp_err_to_name(err));
    	return err;
	}

	err = esp_wifi_set_mode(WIFI_MODE_AP); // Set to Access Point mode
	if (err != ESP_OK) {
    	ESP_LOGE("wifi", "Error setting Wi-Fi mode to AP: %s", esp_err_to_name(err));
    	return err;
	}

	err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
	if (err != ESP_OK) {
    	ESP_LOGE("wifi", "Error setting Wi-Fi configuration for AP: %s", esp_err_to_name(err));
    	return err;
	}

	err = esp_wifi_start();
	if (err != ESP_OK) {
    	ESP_LOGE("wifi", "Error starting Wi-Fi in AP mode: %s", esp_err_to_name(err));
    	return err;
	}

	ESP_LOGI("wifi", "AP Started on SSID: %s", AP_SSID);
	return ESP_OK;
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

    // Create and initialize the station interface
    esp_netif_t *sta_handle = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(sta_handle == NULL ? ESP_FAIL : ESP_OK);

    // Initialize Wi-Fi configuration for STA mode (already provided as `sta_config`)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Create and initialize the Access Point interface
    esp_netif_t *ap_handle = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(ap_handle == NULL ? ESP_FAIL : ESP_OK);

    // Initialize Wi-Fi configuration for AP mode (already provided as `ap_config`)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize Wi-Fi based on the mode
    wifi_mode_t wifi_mode = WIFI_MODE_APSTA; // Change to WIFI_MODE_AP or WIFI_MODE_STA_AP as needed
    ESP_ERROR_CHECK(esp_wifi_set_mode(wifi_mode));

    // Initialize Station if needed
    if (wifi_mode == WIFI_MODE_STA || wifi_mode == WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(wifi_init_sta());
    }
    // Initialize Access Point if needed
    if (wifi_mode == WIFI_MODE_AP || wifi_mode == WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(wifi_init_ap());
    }

    // Initialize raw socket for packet interception
    raw_socket_init();

    // Define source and destination IPs
    ip_addr_t src_ip, dest_ip;
    IP4_ADDR(&src_ip.u_addr.ip4, 192, 168, 1, 100);  // Source IP
    IP4_ADDR(&dest_ip.u_addr.ip4, 192, 168, 1, 101); // Destination IP

    // Add sample NAT entry with source and destination IPs
    add_nat_entry(src_ip, 12345, dest_ip, 80, translated_ip, translated_port, is_snat, is_dnat);

   
// Continuously monitor packets (in real implementation, this loop should process raw socket data)
while (1) {
    // This is a placeholder for the packet interception loop, where raw socket
    // processing code would go

    // Receive packet from raw socket (assuming 'sock' is already created)
    struct pbuf *p = NULL;
    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    int ret = lwip_recv(sock, &p, sizeof(p), 0);
    if (ret < 0) {
        ESP_LOGE("Packet", "Error receiving packet");
        continue; // Skip this loop iteration and try again
    }

    // Intercept and modify the packet if necessary (applying NAT translation)
    packet_interception(p, inp);

    // After modification, the packet can be forwarded through a raw socket
    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = 0; // Set to 0 for raw sockets
    dest_addr.sin_addr.s_addr = ((struct ip_hdr *)p->payload)->dest.addr; // Modified destination IP

    // Send the modified packet through a raw socket
    int sock_out = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_IP);
    if (sock_out < 0) {
        ESP_LOGE("Packet", "Failed to create raw socket: %d", sock_out);
        continue; // Skip this loop iteration and try again
    }
    lwip_sendto(sock_out, p->payload, p->len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    lwip_close(sock_out);

    // Free the received packet after processing
    pbuf_free(p);

    // Add a small delay to prevent blocking the system
    vTaskDelay(pdMS_TO_TICKS(10)); // Delay in ms, adjust as needed
}
}
