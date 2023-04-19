/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2023 Xinyu Yang.
 * Copyright(c) 2010-2015 Intel Corporation
 */

#ifndef _MQNIC_ETHDEV_H_
#define _MQNIC_ETHDEV_H_

#include "mqnic_osdep.h"
#include "mqnic_regs.h"

#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>
#include <netinet/in.h>

#include <rte_string_fns.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_io.h>
#include <rte_interrupts.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_pci.h>
#include <rte_bus_pci.h>
#include <rte_branch_prediction.h>
#include <rte_memory.h>
#include <rte_kvargs.h>
#include <rte_eal.h>
#include <rte_alarm.h>
#include <rte_ether.h>
#include <rte_ethdev_driver.h>
#include <rte_ethdev_pci.h>
#include <rte_malloc.h>
#include <rte_random.h>
#include <rte_mempool.h>
#include <rte_dev.h>
#include <rte_flow.h>
#include <rte_time.h>

#define MQNIC_INTEL_VENDOR_ID 0x1234

/*
 * Defines that were not part of mqnic_hw.h as they are not used by the FreeBSD
 * driver.
 */
#define MQNIC_ADVTXD_POPTS_TXSM     0x00000200 /* L4 Checksum offload request */
#define MQNIC_ADVTXD_POPTS_IXSM     0x00000100 /* IP Checksum offload request */
#define MQNIC_ADVTXD_TUCMD_L4T_RSV  0x00001800 /* L4 Packet TYPE of Reserved */
#define MQNIC_RXD_STAT_TMST         0x10000    /* Timestamped Packet indication */
#define MQNIC_RXD_ERR_CKSUM_BIT     29
#define MQNIC_RXD_ERR_CKSUM_MSK     3
#define MQNIC_ADVTXD_MACLEN_SHIFT   9          /* Bit shift for l2_len */
#define MQNIC_CTRL_EXT_EXTEND_VLAN  (1<<26)    /* EXTENDED VLAN */
#define IGB_VFTA_SIZE 128

#define IGB_HKEY_MAX_INDEX             10
#define IGB_MAX_RX_QUEUE_NUM           8
#define IGB_MAX_RX_QUEUE_NUM_82576     16


#define IGB_RSS_OFFLOAD_ALL ( \
	ETH_RSS_IPV4 | \
	ETH_RSS_NONFRAG_IPV4_TCP | \
	ETH_RSS_NONFRAG_IPV4_UDP | \
	ETH_RSS_IPV6 | \
	ETH_RSS_NONFRAG_IPV6_TCP | \
	ETH_RSS_NONFRAG_IPV6_UDP | \
	ETH_RSS_IPV6_EX | \
	ETH_RSS_IPV6_TCP_EX | \
	ETH_RSS_IPV6_UDP_EX)

/*
 * The overhead from MTU to max frame size.
 * Considering VLAN so a tag needs to be counted.
 */
#define MQNIC_ETH_OVERHEAD (RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN + \
				VLAN_TAG_SIZE)

/*
 * Maximum number of Ring Descriptors.
 *
 * Since RDLEN/TDLEN should be multiple of 128 bytes, the number of ring
 * desscriptors should meet the following condition:
 * (num_ring_desc * sizeof(struct mqnic_rx/tx_desc)) % 128 == 0
 */
#define	MQNIC_MIN_RING_DESC	32
#define	MQNIC_MAX_RING_DESC	1024

/*
 * TDBA/RDBA should be aligned on 16 byte boundary. But TDLEN/RDLEN should be
 * multiple of 128 bytes. So we align TDBA/RDBA on 128 byte boundary.
 * This will also optimize cache line size effect.
 * H/W supports up to cache line size 128.
 */
#define	MQNIC_ALIGN	128

//#define	IGB_RXD_ALIGN	(MQNIC_ALIGN / sizeof(union mqnic_adv_rx_desc))
//#define	IGB_TXD_ALIGN	(MQNIC_ALIGN / sizeof(union mqnic_adv_tx_desc))

#define	IGB_RXD_ALIGN	(MQNIC_ALIGN / MQNIC_DESC_SIZE)
#define	IGB_TXD_ALIGN	(MQNIC_ALIGN / MQNIC_DESC_SIZE)

#define IGB_TX_MAX_SEG     UINT8_MAX
#define IGB_TX_MAX_MTU_SEG UINT8_MAX
#define EM_TX_MAX_SEG      UINT8_MAX
#define EM_TX_MAX_MTU_SEG  UINT8_MAX

/* 802.1q VLAN Packet Size */
#define VLAN_TAG_SIZE			4    /* 802.3ac tag (not DMA'd) */
#define MQNIC_VLAN_FILTER_TBL_SIZE	128  /* VLAN Filter Table (4096 bits) */

/* Error Codes */
#define MQNIC_SUCCESS			0
#define MQNIC_ERR_NVM			1
#define MQNIC_ERR_PHY			2
#define MQNIC_ERR_CONFIG		3
#define MQNIC_ERR_PARAM			4
#define MQNIC_ERR_MAC_INIT		5
#define MQNIC_ERR_PHY_TYPE		6
#define MQNIC_ERR_RESET			9
#define MQNIC_ERR_MASTER_REQUESTS_PENDING	10
#define MQNIC_ERR_HOST_INTERFACE_COMMAND	11
#define MQNIC_BLK_PHY_RESET		12
#define MQNIC_ERR_SWFW_SYNC		13
#define MQNIC_NOT_IMPLEMENTED		14
#define MQNIC_ERR_MBX			15
#define MQNIC_ERR_INVALID_ARGUMENT	16
#define MQNIC_ERR_NO_SPACE		17
#define MQNIC_ERR_NVM_PBA_SECTION	18
#define MQNIC_ERR_I2C			19
#define MQNIC_ERR_INVM_VALUE_NOT_FOUND	20


#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

#define ETH_HLEN 14

#ifndef ilog2
static inline int rss_ilog2(uint32_t x)
{
	int log = 0;
	x >>= 1;

	while (x) {
		log++;
		x >>= 1;
	}
	return log;
}
#define ilog2(x) rss_ilog2(x)

static inline uint32_t fls(uint32_t x)
{
	uint32_t position;
	uint32_t i;

	if (x == 0)
		return 0;

	for (i = (x >> 1), position = 0; i != 0; ++position)
		i >>= 1;

	return position + 1;
}

static inline uint32_t roundup_pow_of_two(uint32_t x)
{
	return 1UL << fls(x - 1);
}

#endif


/* Function pointers for the MAC. */
struct mqnic_mac_operations {
	s32  (*init_params)(struct mqnic_hw *);
	s32  (*id_led_init)(struct mqnic_hw *);
	s32  (*blink_led)(struct mqnic_hw *);
	bool (*check_mng_mode)(struct mqnic_hw *);
	s32  (*check_for_link)(struct mqnic_hw *);
	s32  (*cleanup_led)(struct mqnic_hw *);
	void (*clear_hw_cntrs)(struct mqnic_hw *);
	void (*clear_vfta)(struct mqnic_hw *);
	s32  (*get_bus_info)(struct mqnic_hw *);
	void (*set_lan_id)(struct mqnic_hw *);
	s32  (*get_link_up_info)(struct mqnic_hw *, u16 *, u16 *);
	s32  (*led_on)(struct mqnic_hw *);
	s32  (*led_off)(struct mqnic_hw *);
	void (*update_mc_addr_list)(struct mqnic_hw *, u8 *, u32);
	s32  (*reset_hw)(struct mqnic_hw *);
	s32  (*init_hw)(struct mqnic_hw *);
	void (*shutdown_serdes)(struct mqnic_hw *);
	void (*power_up_serdes)(struct mqnic_hw *);
	s32  (*setup_link)(struct mqnic_hw *);
	s32  (*setup_physical_interface)(struct mqnic_hw *);
	s32  (*setup_led)(struct mqnic_hw *);
	void (*write_vfta)(struct mqnic_hw *, u32, u32);
	void (*config_collision_dist)(struct mqnic_hw *);
	int  (*rar_set)(struct mqnic_hw *, u8*, u32);
	s32  (*read_mac_addr)(struct mqnic_hw *);
	s32  (*validate_mdi_setting)(struct mqnic_hw *);
	s32  (*acquire_swfw_sync)(struct mqnic_hw *, u16);
	void (*release_swfw_sync)(struct mqnic_hw *, u16);
};

struct mqnic_mac_info {
	struct mqnic_mac_operations ops;
	u8 addr[ETH_ADDR_LEN];
	u8 perm_addr[ETH_ADDR_LEN];

	//enum mqnic_mac_type type;

	u32 collision_delta;
	u32 ledctl_default;
	u32 ledctl_mode1;
	u32 ledctl_mode2;
	u32 mc_filter_type;
	u32 tx_packet_delta;
	u32 txcw;

	u16 current_ifs_val;
	u16 ifs_max_val;
	u16 ifs_min_val;
	u16 ifs_ratio;
	u16 ifs_step_size;
	u16 mta_reg_count;
	u16 uta_reg_count;

	/* Maximum size of the MTA register table in all supported adapters */
#define MAX_MTA_REG 128
	u32 mta_shadow[MAX_MTA_REG];
	u16 rar_entry_count;

	u8  forced_speed_duplex;

	bool adaptive_ifs;
	bool has_fwsm;
	bool arc_subsystem_valid;
	bool asf_firmware_present;
	bool autoneg;
	bool autoneg_failed;
	bool get_link_status;
	bool in_ifs_mode;
	bool report_tx_early;
	//enum mqnic_serdes_link_state serdes_link_state;
	bool serdes_has_link;
	bool tx_pkt_filtering;
};

struct mqnic_hw {
	void *back;

	u8 *flash_address;
	unsigned long io_base;

	struct mqnic_mac_info  mac;

	struct mqnic_reg_block *rb_list;
	struct mqnic_reg_block *fw_id_rb;
	struct mqnic_reg_block *if_rb;
	struct mqnic_reg_block *phc_rb;

	struct mqnic_if *interface[MQNIC_MAX_IF];

	u16 device_id;
	u16 subsystem_vendor_id;
	u16 subsystem_device_id;
	u16 vendor_id;

	u8  revision_id;

	//corundum add
	uint64_t hw_regs_size;
	phys_addr_t hw_regs_phys;
	u8 *hw_addr;
	u8 *phc_hw_addr;

	u8 base_mac[ETH_ALEN];

	u32 fpga_id;
	u32 fw_id;
	u32 fw_ver;
	u32 board_id;
	u32 board_ver;
	u32 build_date;
	u32 git_hash;
	u32 rel_info;

	u32 app_id;

	u32 if_offset;
	u32 if_count;
	u32 if_stride;
	u32 if_csr_offset;
};

struct mqnic_if {
	struct mqnic_hw *hw;

	struct mqnic_reg_block *rb_list;
	struct mqnic_reg_block *if_ctrl_rb;
	struct mqnic_reg_block *event_queue_rb;
	struct mqnic_reg_block *tx_queue_rb;
	struct mqnic_reg_block *tx_cpl_queue_rb;
	struct mqnic_reg_block *rx_queue_rb;
	struct mqnic_reg_block *rx_cpl_queue_rb;
	struct mqnic_reg_block *rx_queue_map_rb;

	int index;

	int dev_port_base;
	int dev_port_max;
	int dev_port_limit;

	u32 if_features;

	u32 max_tx_mtu;
	u32 max_rx_mtu;

	u32 event_queue_offset;
	u32 event_queue_count;
	u32 event_queue_stride;
	struct mqnic_eq_ring *event_ring[MQNIC_MAX_EVENT_RINGS];

	u32 tx_queue_offset;
	u32 tx_queue_count;
	u32 tx_queue_stride;
	struct mqnic_ring *tx_ring[MQNIC_MAX_TX_RINGS];

	u32 tx_cpl_queue_offset;
	u32 tx_cpl_queue_count;
	u32 tx_cpl_queue_stride;
	struct mqnic_cq_ring *tx_cpl_ring[MQNIC_MAX_TX_CPL_RINGS];

	u32 rx_queue_offset;
	u32 rx_queue_count;
	u32 rx_queue_stride;
	struct mqnic_ring *rx_ring[MQNIC_MAX_RX_RINGS];

	u32 rx_cpl_queue_offset;
	u32 rx_cpl_queue_count;
	u32 rx_cpl_queue_stride;
	struct mqnic_cq_ring *rx_cpl_ring[MQNIC_MAX_RX_CPL_RINGS];

	u32 port_count;
	struct mqnic_port *port[MQNIC_MAX_PORTS];

	u32 sched_block_count;
	struct mqnic_sched_block *sched_block[MQNIC_MAX_PORTS];

	u32 max_desc_block_size;

	size_t hw_regs_size;
	u8 *hw_addr;
	u8 *csr_hw_addr;

	u32 ndev_count;
	struct net_device *ndev[MQNIC_MAX_PORTS];

	struct i2c_client *mod_i2c_client;
};

struct mqnic_port {
    struct device *dev;
    struct net_device *ndev;

    int index;

    uint32_t tx_queue_count;

    uint32_t port_id;
    uint32_t port_features;
    uint32_t port_mtu;
    uint32_t sched_count;
    uint32_t sched_offset;
    uint32_t sched_stride;
    uint32_t sched_type;

    u8 *hw_addr;
};


struct mqnic_priv {
    //spinlock_t stats_lock;

    bool registered;
    int port;
    bool port_up;

    uint32_t if_id;
    uint32_t if_features;
    uint32_t event_queue_count;
    uint32_t event_queue_offset;
    uint32_t tx_queue_count;
    uint32_t tx_queue_offset;
    uint32_t tx_cpl_queue_count;
    uint32_t tx_cpl_queue_offset;
    uint32_t rx_queue_count;
    uint32_t rx_queue_offset;
    uint32_t rx_cpl_queue_count;
    uint32_t rx_cpl_queue_offset;
    uint32_t port_count;
    uint32_t port_offset;
    uint32_t port_stride;

	uint32_t desc_block_size;
    uint32_t max_desc_block_size;

	uint64_t ipackets;  /**< Total number of successfully received packets. */
	uint64_t opackets;  /**< Total number of successfully transmitted packets.*/
	uint64_t ibytes;    /**< Total number of successfully received bytes. */
	uint64_t obytes;    /**< Total number of successfully transmitted bytes. */

    u8 *hw_addr;
    u8 *csr_hw_addr;

    struct mqnic_eq_ring *event_ring[MQNIC_MAX_EVENT_RINGS];
    struct mqnic_ring *tx_ring[MQNIC_MAX_TX_RINGS];
    struct mqnic_cq_ring *tx_cpl_ring[MQNIC_MAX_TX_CPL_RINGS];
    struct mqnic_ring *rx_ring[MQNIC_MAX_RX_RINGS];
    struct mqnic_cq_ring *rx_cpl_ring[MQNIC_MAX_RX_CPL_RINGS];
    struct mqnic_port *ports[MQNIC_MAX_PORTS];
};

/*
 * Structure to store private data for each driver instance (for each port).
 */
struct mqnic_adapter {
	struct mqnic_hw         hw;
	struct mqnic_priv priv;
	//struct mqnic_hw_stats   stats;
	//struct mqnic_interrupt  intr;
	//struct mqnic_filter_info filter;
	bool stopped;
	//struct rte_timecounter  systime_tc;
	//struct rte_timecounter  rx_tstamp_tc;
	//struct rte_timecounter  tx_tstamp_tc;
};

struct mqnic_desc {
	u16 rsvd0;
	u16 tx_csum_cmd;
	u32 len;
	u64 addr;
};

struct mqnic_cpl {
	u16 queue;
	u16 index;
	u16 len;
	u16 rsvd0;
	u32 ts_ns;
	u16 ts_s;
	u16 rx_csum;
	u32 rx_hash;
	u8 rx_hash_type;
	u8 rsvd1;
	u8 rsvd2;
	u8 rsvd3;
	u32 rsvd4;
	u32 rsvd5;
};

struct mqnic_event {
	u16 type;
	u16 source;
};


/* structure for interrupt relative data */
struct mqnic_interrupt {
	uint32_t flags;
	uint32_t mask;
};

struct mqnic_ring {
	// written on enqueue (i.e. start_xmit)
	uint32_t head_ptr;
	uint64_t bytes;
	uint64_t packets;
	uint64_t dropped_packets;
	struct netdev_queue *tx_queue;

	// written from completion
	uint32_t tail_ptr;
	uint32_t clean_tail_ptr;
	uint64_t ts_s;
	u8 ts_valid;

	// mostly constant
	uint32_t size;  //number of desc
	uint32_t full_size;
	uint32_t size_mask;
	uint32_t stride;

	uint32_t cpl_index;

	uint32_t mtu;
	uint32_t page_order;

	uint32_t desc_block_size;
	uint32_t log_desc_block_size;

	size_t buf_size;
	u8 *buf;
	uint64_t buf_dma_addr;

	//union {
       //     struct mqnic_tx_info *tx_info;
       //     struct mqnic_rx_info *rx_info;
	//};

	uint32_t hw_ptr_mask;
	u8 *hw_addr;
	u8 *hw_head_ptr;
	u8 *hw_tail_ptr;
};
// ____cacheline_aligned_in_smp;

struct mqnic_cq_ring {
    uint32_t head_ptr;

    uint32_t tail_ptr;

    uint32_t size;
    uint32_t size_mask;
    uint32_t stride;

    size_t buf_size;
    u8 *buf;
    uint64_t buf_dma_addr;

    //struct net_device *ndev;
   // struct napi_struct napi;
    int ring_index;
    int eq_index;

    void (*handler) (struct mqnic_cq_ring *);

    uint32_t hw_ptr_mask;
    u8 *hw_addr;
    u8 *hw_head_ptr;
    u8 *hw_tail_ptr;
};

struct mqnic_eq_ring {
    uint32_t head_ptr;

    uint32_t tail_ptr;

    uint32_t size;
    uint32_t size_mask;
    uint32_t stride;

    size_t buf_size;
    u8 *buf;
    uint64_t buf_dma_addr;

    //struct net_device *ndev;
    int int_index;

    int irq;

    void (*handler) (struct mqnic_eq_ring *);

    uint32_t hw_ptr_mask;
    u8 *hw_addr;
    u8 *hw_head_ptr;
    u8 *hw_tail_ptr;
};


#define MQNIC_DEV_PRIVATE(adapter) \
	((struct mqnic_adapter *)adapter)

#define MQNIC_DEV_PRIVATE_TO_HW(adapter) \
	(&((struct mqnic_adapter *)adapter)->hw)

#define MQNIC_DEV_PRIVATE_TO_PRIV(adapter) \
	(&((struct mqnic_adapter *)adapter)->priv)

#define MQNIC_DEV_PRIVATE_TO_STATS(adapter) \
	(&((struct mqnic_adapter *)adapter)->stats)

#define MQNIC_DEV_PRIVATE_TO_INTR(adapter) \
	(&((struct mqnic_adapter *)adapter)->intr)

#define MQNIC_DEV_PRIVATE_TO_VFTA(adapter) \
	(&((struct mqnic_adapter *)adapter)->shadow_vfta)

#define MQNIC_DEV_PRIVATE_TO_P_VFDATA(adapter) \
        (&((struct mqnic_adapter *)adapter)->vfdata)

#define MQNIC_DEV_PRIVATE_TO_FILTER_INFO(adapter) \
	(&((struct mqnic_adapter *)adapter)->filter)


struct mqnic_reg_block {
	u32 type;
	u32 version;
	u8 *regs;
	u8 *base;
};


/*
 * Register block manipulations
 */
struct mqnic_reg_block *mqnic_enumerate_reg_block_list(struct mqnic_hw*, size_t offset, size_t size);
struct mqnic_reg_block *mqnic_find_reg_block(struct mqnic_reg_block *list, u32 type, u32 version, int index);
void mqnic_free_reg_block_list(struct mqnic_reg_block *list);

/*
 * RX/TX IGB function prototypes
 */
void eth_mqnic_tx_queue_release(void *txq);
void eth_mqnic_rx_queue_release(void *rxq);
void mqnic_dev_clear_queues(struct rte_eth_dev *dev);
void mqnic_dev_free_queues(struct rte_eth_dev *dev);
void mqnic_dev_deactive_queues(struct rte_eth_dev *dev);

uint64_t mqnic_get_rx_port_offloads_capa(struct rte_eth_dev *dev);
uint64_t mqnic_get_rx_queue_offloads_capa(struct rte_eth_dev *dev);

int eth_mqnic_rx_queue_setup(struct rte_eth_dev *dev, uint16_t rx_queue_id,
		uint16_t nb_rx_desc, unsigned int socket_id,
		const struct rte_eth_rxconf *rx_conf,
		struct rte_mempool *mb_pool);
uint32_t eth_mqnic_rx_queue_count(struct rte_eth_dev *dev, uint16_t rx_queue_id);

int eth_mqnic_rx_descriptor_done(void *rx_queue, uint16_t offset);
int eth_mqnic_rx_descriptor_status(void *rx_queue, uint16_t offset);
int eth_mqnic_tx_descriptor_status(void *tx_queue, uint16_t offset);

uint64_t mqnic_get_tx_port_offloads_capa(struct rte_eth_dev *dev);
uint64_t mqnic_get_tx_queue_offloads_capa(struct rte_eth_dev *dev);
int eth_mqnic_tx_queue_setup(struct rte_eth_dev *dev, uint16_t tx_queue_id,
		uint16_t nb_tx_desc, unsigned int socket_id,
		const struct rte_eth_txconf *tx_conf);
int eth_mqnic_tx_done_cleanup(void *txq, uint32_t free_cnt);

int eth_mqnic_rx_init(struct rte_eth_dev *dev);
void eth_mqnic_tx_init(struct rte_eth_dev *dev);

uint16_t eth_mqnic_xmit_pkts(void *txq, struct rte_mbuf **tx_pkts, uint16_t nb_pkts);
//uint16_t eth_mqnic_prep_pkts(void *txq, struct rte_mbuf **tx_pkts,
//		uint16_t nb_pkts);
uint16_t eth_mqnic_recv_pkts(void *rxq, struct rte_mbuf **rx_pkts, uint16_t nb_pkts);
uint16_t eth_mqnic_recv_scattered_pkts(void *rxq,
		struct rte_mbuf **rx_pkts, uint16_t nb_pkts);

int eth_mqnic_rss_hash_update(struct rte_eth_dev *dev, struct rte_eth_rss_conf *rss_conf);
int eth_mqnic_rss_hash_conf_get(struct rte_eth_dev *dev, struct rte_eth_rss_conf *rss_conf);
int eth_mqnicvf_rx_init(struct rte_eth_dev *dev);
void eth_mqnicvf_tx_init(struct rte_eth_dev *dev);

/*
 * misc function prototypes
 */
void mqnic_pf_host_init(struct rte_eth_dev *eth_dev);

void mqnic_pf_mbx_process(struct rte_eth_dev *eth_dev);

int mqnic_pf_host_configure(struct rte_eth_dev *eth_dev);

void mqnic_rxq_info_get(struct rte_eth_dev *dev, uint16_t queue_id,
	struct rte_eth_rxq_info *qinfo);

void mqnic_txq_info_get(struct rte_eth_dev *dev, uint16_t queue_id,
	struct rte_eth_txq_info *qinfo);

uint32_t em_get_max_pktlen(struct rte_eth_dev *dev);

/*
 * RX/TX EM function prototypes
 */
void eth_em_tx_queue_release(void *txq);
void eth_em_rx_queue_release(void *rxq);

void em_dev_clear_queues(struct rte_eth_dev *dev);
void em_dev_free_queues(struct rte_eth_dev *dev);

uint64_t em_get_rx_port_offloads_capa(struct rte_eth_dev *dev);
uint64_t em_get_rx_queue_offloads_capa(struct rte_eth_dev *dev);

int eth_em_rx_queue_setup(struct rte_eth_dev *dev, uint16_t rx_queue_id,
		uint16_t nb_rx_desc, unsigned int socket_id,
		const struct rte_eth_rxconf *rx_conf,
		struct rte_mempool *mb_pool);
uint32_t eth_em_rx_queue_count(struct rte_eth_dev *dev, uint16_t rx_queue_id);

int eth_em_rx_descriptor_done(void *rx_queue, uint16_t offset);
int eth_em_rx_descriptor_status(void *rx_queue, uint16_t offset);
int eth_em_tx_descriptor_status(void *tx_queue, uint16_t offset);

uint64_t em_get_tx_port_offloads_capa(struct rte_eth_dev *dev);
uint64_t em_get_tx_queue_offloads_capa(struct rte_eth_dev *dev);
int eth_em_tx_queue_setup(struct rte_eth_dev *dev, uint16_t tx_queue_id,
		uint16_t nb_tx_desc, unsigned int socket_id,
		const struct rte_eth_txconf *tx_conf);

int eth_em_rx_init(struct rte_eth_dev *dev);
void eth_em_tx_init(struct rte_eth_dev *dev);
uint16_t eth_em_xmit_pkts(void *tx_queue, struct rte_mbuf **tx_pkts, uint16_t nb_pkts);
uint16_t eth_em_prep_pkts(void *txq, struct rte_mbuf **tx_pkts, uint16_t nb_pkts);
uint16_t eth_em_recv_pkts(void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t nb_pkts);
uint16_t eth_em_recv_scattered_pkts(void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t nb_pkts);

void em_rxq_info_get(struct rte_eth_dev *dev, uint16_t queue_id, struct rte_eth_rxq_info *qinfo);
void em_txq_info_get(struct rte_eth_dev *dev, uint16_t queue_id, struct rte_eth_txq_info *qinfo);

void mqnic_pf_host_uninit(struct rte_eth_dev *dev);
void mqnic_filterlist_flush(struct rte_eth_dev *dev);

struct mqnic_frag {
    uint64_t dma_addr;
    uint32_t len;
};

void mqnic_arm_cq(struct mqnic_cq_ring *ring);
void mqnic_cpl_queue_release(struct mqnic_cq_ring *ring);


#endif /* _MQNIC_ETHDEV_H_ */
