# Open lwIP with NAT, SLIP, and static routing for the ESP8266

## NAT
The new functions are exported in the "lwip/lwip_napt.h" header. NAPT should be enabled on the SoftAP interface of the ESP (this is typically the interface with netif.num == 1).

The additional NAPT functionality is enabled by the options IP_FORWARD, IP_NAPT, and IP_NAPT_DYNAMIC in "lwipopts.h".

This version of lwIP can replace the default version of lwIP in the esp-open-sdk. Just clone and replace the esp-lwip-open directory in the sdk.

It is based on neocat's NAPT extensions: https://github.com/NeoCat/esp8266-Arduino/commit/4108c8dbced7769c75bcbb9ed880f1d3f178bcbe

Fixes some issues I had with checksums and timers and can be used for full WiFi repeater functionality. If you want to use it as a permanent replacement for liblwib.a you might want to add the option IP_NAPT_DYNAMIC = 1. With this option the memory for the NAPT tables is allocated only, if ip_napt_init(max_nat, max_portmap) is called explicitly before enabling NAPT.

## SLIP
This stack also supports SLIP (Serial Line IP) interfaces via UARTs. To get this up and running, you will need an appropriate UART-driver and some initialization in the main program. You can find a demo at: https://github.com/martin-ger/esp_slip_router

## ENC28J60 Ethernet
Starting from the Ethernet ENC28J60 driver from https://github.com/Informatic/espenc . This works with an ENC28J60 connected via SPI. To get this running, you will need at least this SPI driver: https://github.com/MetalPhreak/ESP8266_SPI_Driver and the following wiring:
```
ESP8266      ENC28J60

GPIO12 <---> MISO
GPIO13 <---> MOSI
GPIO14 <---> SCLK
GPIO15 <---> CS
GPIO5  <---> INT
GPIO4  <---> RESET
Q3/V33 <---> 3.3V
GND    <---> GND
```
In addition you will need a transistor for decoupling GPIO15, otherwise your ESP will not boot any more, see: https://esp8266hints.wordpress.com/category/ethernet/

Usage:
Simply connect the enc28j60 as described above, include "lwip/netif.h" and "netif/espenc.h" and enable by setting the option LWIP_HAVE_ENC28J60IF in "lwipopts.h".
A hardware reset in the ESP's init is optional, but it ensures that an ESP reset also resets the enc. It is done by toggeling GPIO4 (0 and 1), e.g. using the easygpio lib:
```
#define ENC28J60_HW_RESET 4

easygpio_pinMode(ENC28J60_HW_RESET, EASYGPIO_PULLUP, EASYGPIO_OUTPUT);
easygpio_outputSet(ENC28J60_HW_RESET, 0);
os_delay_us(500);
easygpio_outputSet(ENC28J60_HW_RESET, 1);
os_delay_us(1000);
```

Initialize the Ethernet interface in your code by calling the init function:
```
struct netif* espenc_init(uint8_t *mac_addr, ip_addr_t *ip, ip_addr_t *mask, ip_addr_t *gw, bool dhcp);
```

## Static Routing Table

IPv4 now has a static routing table. In "ip_route.h" there are these new functions:
```
struct route_entry {
    ip_addr_t ip;
    ip_addr_t mask;
    ip_addr_t gw;
};

/* Add a static route, true on success */
bool ip_add_route(ip_addr_t ip, ip_addr_t mask, ip_addr_t gw);

/* Remove a static route, true on success */
bool ip_rm_route(ip_addr_t ip, ip_addr_t mask);

/* Finds a route entry for an address, NULL if none */
struct route_entry *ip_find_route(ip_addr_t ip);

/* Delete all static routes */
void ip_delete_routes(void);

/* Returns the n_th entry of the routing table, true on success */
bool ip_get_route(uint32_t no, ip_addr_t *ip, ip_addr_t *mask, ip_addr_t *gw);
```

## Additional Netifs

### Loopback
With LWIP_HAVE_LOOPIF 1 in lwipopts.h the lwip stack provides a loopback ("lo0") interface at 127.0.0.1. To get it working call void loopback_netif_init(netif_status_callback_fn cb) from netif.h. Either the callback provided in the init function is implemented to schedule a netif_poll(netif) in the main task or (with NULL callback) netif_poll_all() has to be called peroidically in the main loop. 

### Tunif
A TUNIF dummy device: this skeleton driver needs at least some additional load/unload functions to be useful for anything. It is intended as starting point for a tunnel device, e.g. for some kind of VPN tunnel.
