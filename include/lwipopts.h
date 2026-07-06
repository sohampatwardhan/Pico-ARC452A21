#pragma once

#define NO_SYS 1
#define LWIP_RAW 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_ICMP 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define TCP_MSS 1460
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)
#define PBUF_POOL_SIZE 24
#define MEMP_NUM_TCP_PCB 6
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_UDP_PCB 6
#define MEMP_NUM_SYS_TIMEOUT 16
#define DHCP_DOES_ARP_CHECK 0
