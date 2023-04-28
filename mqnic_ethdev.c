/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2023 Xinyu Yang.
 * Copyright (c) 2022 Bruce.
 */

#include "mqnic.h"
#include "mqnic_logs.h"
#include "mqnic_regs.h"

uint32_t event_queue_size = 1024;   //number of event queue
uint32_t cpl_queue_size = 1024;   //number of event queue

/*
 * Default values for port configuration
 */
#define IGB_DEFAULT_RX_FREE_THRESH  32

#define IGB_DEFAULT_RX_PTHRESH     8
#define IGB_DEFAULT_RX_HTHRESH      8
#define IGB_DEFAULT_RX_WTHRESH     4

#define IGB_DEFAULT_TX_PTHRESH     8
#define IGB_DEFAULT_TX_HTHRESH      1
#define IGB_DEFAULT_TX_WTHRESH     16

/* External VLAN Enable bit mask */
#define MQNIC_CTRL_EXT_EXT_VLAN      (1 << 26)

/* MSI-X other interrupt vector */
#define IGB_MSIX_OTHER_INTR_VEC      0

static int  eth_mqnic_configure(struct rte_eth_dev *dev);
static int  eth_mqnic_start(struct rte_eth_dev *dev);
static int  eth_mqnic_stop(struct rte_eth_dev *dev);
static int eth_mqnic_close(struct rte_eth_dev *dev);
static int eth_mqnic_reset(struct rte_eth_dev *dev);
static int  eth_mqnic_promiscuous_enable(struct rte_eth_dev *dev);
static int  eth_mqnic_promiscuous_disable(struct rte_eth_dev *dev);
static int  eth_mqnic_link_update(struct rte_eth_dev *dev,
				int wait_to_complete);

static int eth_mqnic_stats_get(struct rte_eth_dev *dev,
				struct rte_eth_stats *rte_stats);
static int eth_mqnic_stats_reset(struct rte_eth_dev *dev);
static int eth_mqnic_infos_get(struct rte_eth_dev *dev,
			      struct rte_eth_dev_info *dev_info);
static const uint32_t *eth_mqnic_supported_ptypes_get(struct rte_eth_dev *dev);
static int  eth_mqnic_mtu_set(struct rte_eth_dev *dev, uint16_t mtu);

/*
 * Define VF Stats MACRO for Non "cleared on read" register
 */
#define UPDATE_VF_STAT(reg, last, cur)            \
{                                                 \
	u32 latest = MQNIC_READ_REG(hw, reg);     \
	cur += (latest - last) & UINT_MAX;        \
	last = latest;                            \
}

#define IGB_FC_PAUSE_TIME 0x0680
#define IGB_LINK_UPDATE_CHECK_TIMEOUT  90  /* 9s */
#define IGB_LINK_UPDATE_CHECK_INTERVAL 100 /* ms */

#define IGBVF_PMD_NAME "rte_igbvf_pmd"     /* PMD name */

/*
 * The set of PCI devices this driver supports
 */
static const struct rte_pci_id pci_id_mqnic_map[] = {
	{ RTE_PCI_DEVICE(MQNIC_INTEL_VENDOR_ID, MQNIC_DEV_ID) },
	{ .vendor_id = 0, /* sentinel */ },
};

static const struct rte_eth_desc_lim rx_desc_lim = {
	.nb_max = MQNIC_MAX_RING_DESC,
	.nb_min = MQNIC_MIN_RING_DESC,
	.nb_align = IGB_RXD_ALIGN,
};

static const struct rte_eth_desc_lim tx_desc_lim = {
	.nb_max = MQNIC_MAX_RING_DESC,
	.nb_min = MQNIC_MIN_RING_DESC,
	.nb_align = IGB_RXD_ALIGN,
	.nb_seg_max = IGB_TX_MAX_SEG,
	.nb_mtu_seg_max = IGB_TX_MAX_MTU_SEG,
};

static const struct eth_dev_ops eth_mqnic_ops = {
	.dev_configure        = eth_mqnic_configure,
	.dev_start            = eth_mqnic_start,
	.dev_stop             = eth_mqnic_stop,
	.dev_close            = eth_mqnic_close,
	.dev_reset            = eth_mqnic_reset,
	.promiscuous_enable   = eth_mqnic_promiscuous_enable,
	.promiscuous_disable  = eth_mqnic_promiscuous_disable,
	.link_update          = eth_mqnic_link_update,
	.stats_get            = eth_mqnic_stats_get,
	.stats_reset          = eth_mqnic_stats_reset,
	.dev_infos_get        = eth_mqnic_infos_get,
	.dev_supported_ptypes_get = eth_mqnic_supported_ptypes_get,
	.mtu_set              = eth_mqnic_mtu_set,
	.rx_queue_setup       = eth_mqnic_rx_queue_setup,
	.rx_queue_release     = eth_mqnic_rx_queue_release,
	.tx_queue_setup       = eth_mqnic_tx_queue_setup,
	.tx_queue_release     = eth_mqnic_tx_queue_release,
	.tx_done_cleanup      = eth_mqnic_tx_done_cleanup,
	.rxq_info_get         = mqnic_rxq_info_get,
	.txq_info_get         = mqnic_txq_info_get,
};

static void mqnic_interface_set_rx_queue_map_offset(struct mqnic_if *interface, int port, u32 val)
{
	MQNIC_DIRECT_WRITE_REG(interface->rx_queue_map_rb->regs, MQNIC_RB_RX_QUEUE_MAP_CH_OFFSET +
			MQNIC_RB_RX_QUEUE_MAP_CH_STRIDE*port + MQNIC_RB_RX_QUEUE_MAP_CH_REG_OFFSET, val);
}

static u32 mqnic_interface_get_rx_queue_map_rss_mask(struct mqnic_if *interface, int port)
{
	return MQNIC_DIRECT_READ_REG(interface->rx_queue_map_rb->regs, MQNIC_RB_RX_QUEUE_MAP_CH_OFFSET +
			MQNIC_RB_RX_QUEUE_MAP_CH_STRIDE*port + MQNIC_RB_RX_QUEUE_MAP_CH_REG_RSS_MASK);
}

static void mqnic_interface_set_rx_queue_map_rss_mask(struct mqnic_if *interface, int port, u32 val)
{
	MQNIC_DIRECT_WRITE_REG(interface->rx_queue_map_rb->regs, MQNIC_RB_RX_QUEUE_MAP_CH_OFFSET +
			MQNIC_RB_RX_QUEUE_MAP_CH_STRIDE*port + MQNIC_RB_RX_QUEUE_MAP_CH_REG_RSS_MASK, val);
}

static u32 mqnic_interface_get_rx_queue_map_app_mask(struct mqnic_if *interface, int port)
{
	return MQNIC_DIRECT_READ_REG(interface->rx_queue_map_rb->regs, MQNIC_RB_RX_QUEUE_MAP_CH_OFFSET +
			MQNIC_RB_RX_QUEUE_MAP_CH_STRIDE*port + MQNIC_RB_RX_QUEUE_MAP_CH_REG_APP_MASK);
}

static void mqnic_interface_set_rx_queue_map_app_mask(struct mqnic_if *interface, int port, u32 val)
{
	MQNIC_DIRECT_WRITE_REG(interface->rx_queue_map_rb->regs, MQNIC_RB_RX_QUEUE_MAP_CH_OFFSET +
			MQNIC_RB_RX_QUEUE_MAP_CH_STRIDE*port + MQNIC_RB_RX_QUEUE_MAP_CH_REG_APP_MASK, val);
}

static void
mqnic_event_queue_release(struct mqnic_eq_ring *ring)
{
	if (ring != NULL) {
		rte_free(ring);
	}
}

static int
mqnic_all_event_queue_create(struct mqnic_if *interface)
{
	struct mqnic_eq_ring *ring;
	uint32_t i;
	
	PMD_INIT_LOG(DEBUG, "mqnic_all_event_queue_create");

	for (i = 0; i < interface->event_queue_count; i++){
		/* Free memory prior to re-allocation if needed */
		if (interface->event_ring[i] != NULL) {
			PMD_INIT_LOG(DEBUG, "release event ring %d", i);
			mqnic_event_queue_release(interface->event_ring[i]);
			interface->event_ring[i] = NULL;
		}

		// Create event queue
		ring = rte_zmalloc("ethdev event queue", sizeof(struct mqnic_eq_ring),
							RTE_CACHE_LINE_SIZE);
		if (ring == NULL){
			PMD_INIT_LOG(ERR, "failed to alloc event queue");
			return -ENOMEM;
		}

		ring->interface = interface;
		ring->index = i;
		ring->active = 0;

		ring->hw_addr = interface->hw_addr + interface->event_queue_offset
			+ i * interface->event_queue_stride;
		ring->hw_ptr_mask = 0xffff;
		ring->hw_head_ptr = ring->hw_addr + MQNIC_EVENT_QUEUE_HEAD_PTR_REG;
		ring->hw_tail_ptr = ring->hw_addr + MQNIC_EVENT_QUEUE_TAIL_PTR_REG;

		ring->head_ptr = 0;
		ring->tail_ptr = 0;

		PMD_INIT_LOG(DEBUG, "ring->buf=%p ring->hw_addr=%p ring->buf_dma_addr=0x%"PRIx64,
		     ring->buf, ring->hw_addr, ring->buf_dma_addr);

		// Deactivate queue
		MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_ACTIVE_LOG_SIZE_REG, 0);

		interface->event_ring[i] = ring;
	}

	MQNIC_WRITE_FLUSH(interface);

	return 0;
}

static int
mqnic_all_event_queue_alloc(struct mqnic_if *interface, int socket_id)
{
	const struct rte_memzone *tz;
	struct mqnic_eq_ring *ring;
	uint32_t i;
	struct rte_eth_dev *dev = interface->hw->dev;
	
	PMD_INIT_LOG(DEBUG, "mqnic_all_event_queue_create");

	for (i = 0; i < interface->event_queue_count; i++){
		/* Free memory prior to re-allocation if needed */
		if (interface->event_ring[i]->buf != NULL) {
			return -EINVAL;
		}

		ring = interface->event_ring[i];

		// Allocate event queue
		ring->size = roundup_pow_of_two(event_queue_size);
		ring->size_mask = ring->size - 1;
		ring->stride = roundup_pow_of_two(MQNIC_EVENT_SIZE);

		ring->buf_size = ring->size * ring->stride;
		tz = rte_eth_dma_zone_reserve(dev, "event_ring", i,
				ring->buf_size, MQNIC_ALIGN, socket_id);
		if (tz == NULL) {
			PMD_INIT_LOG(ERR, "failed to alloc event ring buffer, i = %d.", i);
			rte_free(ring);
			return -ENOMEM;
		}
		ring->buf = (u8*)tz->addr;
		ring->buf_dma_addr = tz->iova;
	}

	MQNIC_WRITE_FLUSH(interface);

	return 0;
}

static void
mqnic_all_event_queue_destroy(struct mqnic_if *interface)
{
	struct mqnic_eq_ring *ring;
	uint32_t i;
	struct rte_eth_dev *dev = interface->hw->dev;
	
	PMD_INIT_LOG(DEBUG, "mqnic_all_event_queue_destroy");

	for (i = 0; i < interface->event_queue_count; i++){
		/* Free memory prior to re-allocation if needed */
		if (interface->event_ring[i] != NULL) {
			ring = interface->event_ring[i];
			// deactivate queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_ACTIVE_LOG_SIZE_REG, ilog2(ring->size));
			// disarm queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_INTERRUPT_INDEX_REG, ring->index);
			MQNIC_WRITE_FLUSH(interface);
			PMD_INIT_LOG(DEBUG, "release event ring %d", i);
			mqnic_event_queue_release(interface->event_ring[i]);
			interface->event_ring[i] = NULL;
		}

		rte_eth_dma_zone_free(dev, "event_ring", i);
	}

	return;
}

static void
mqnic_all_event_queue_deactivate(struct mqnic_if *interface)
{
	struct mqnic_eq_ring *ring;
	uint32_t i;
	
	PMD_INIT_LOG(DEBUG, "mqnic_all_event_queue_deactivate");

	for (i = 0; i < interface->event_queue_count; i++){
		/* Free memory prior to re-allocation if needed */
		if (interface->event_ring[i] != NULL) {
			ring = interface->event_ring[i];
			// deactivate queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_ACTIVE_LOG_SIZE_REG, ilog2(ring->size));
			// disarm queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_INTERRUPT_INDEX_REG, ring->irq);
			MQNIC_WRITE_FLUSH(interface);
		}
	}

	return;
}

static void mqnic_arm_eq(struct mqnic_eq_ring *ring)
{
	//PMD_INIT_LOG(DEBUG, "skip arm eq, int_index = %d!!", ring->int_index);
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_INTERRUPT_INDEX_REG, ring->irq | MQNIC_EVENT_QUEUE_ARM_MASK);
}

static int
mqnic_all_event_queue_active(struct mqnic_if *interface)
{
	struct mqnic_eq_ring *ring;
	uint32_t i;
	int int_index = 0; //only one interrupt
	
	PMD_INIT_LOG(DEBUG, "mqnic_all_event_queue_active");

	for (i = 0; i < interface->event_queue_count; i++){
		ring = interface->event_ring[i];
		/* Free memory prior to re-allocation if needed */
		if (ring == NULL) {
			PMD_INIT_LOG(ERR, "invalid event ring buffer, i = %d.", i);
			return -1;
		}
		ring->irq = int_index;

		// deactivate queue
		MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_ACTIVE_LOG_SIZE_REG, 0);

		// set base address
		MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_BASE_ADDR_REG+0, ring->buf_dma_addr);
		MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_BASE_ADDR_REG+4, ring->buf_dma_addr >> 32);
		// set interrupt index
		MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_INTERRUPT_INDEX_REG, ring->irq);
		// set pointers
		MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_HEAD_PTR_REG, ring->head_ptr & ring->hw_ptr_mask);
		MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_TAIL_PTR_REG, ring->tail_ptr & ring->hw_ptr_mask);
		// set size and active mask
		MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_EVENT_QUEUE_ACTIVE_LOG_SIZE_REG, ilog2(ring->size) | MQNIC_EVENT_QUEUE_ACTIVE_MASK);


		mqnic_arm_eq(ring);
	}
	MQNIC_WRITE_FLUSH(interface);

	return 0;
}


static void _create_cpl_queue(struct mqnic_cq_ring *ring, struct mqnic_if *interface, int i) {
	ring->interface = interface;
	ring->index = i;
	ring->active = 0;

	ring->hw_addr = interface->hw_addr + interface->tx_cpl_queue_offset
		+ i * interface->tx_cpl_queue_stride;
	ring->hw_ptr_mask = 0xffff;
	ring->hw_head_ptr = ring->hw_addr + MQNIC_CPL_QUEUE_HEAD_PTR_REG;
	ring->hw_tail_ptr = ring->hw_addr + MQNIC_CPL_QUEUE_TAIL_PTR_REG;

	ring->head_ptr = 0;
	ring->tail_ptr = 0;

	PMD_INIT_LOG(DEBUG, "ring->buf=%p ring->hw_addr=%p ring->buf_dma_addr=0x%"PRIx64,
	     ring->buf, ring->hw_addr, ring->buf_dma_addr);
}

static int _alloc_cpl_queue(struct mqnic_cq_ring *ring, struct rte_eth_dev *dev, int i, int socket_id) {
	const struct rte_memzone *tz;
	ring->size = roundup_pow_of_two(cpl_queue_size);
	ring->size_mask = ring->size - 1;
	ring->stride = roundup_pow_of_two(MQNIC_CPL_SIZE);

	ring->buf_size = ring->size * ring->stride;
	tz = rte_eth_dma_zone_reserve(dev, "cq_ring", i, ring->buf_size,
			      MQNIC_ALIGN, socket_id);
	if (tz == NULL) {
		PMD_INIT_LOG(ERR, "failed to alloc cq ring buffer, i = %d, buf_size = 0x%lx, size = 0x%x, stride = 0x%x", i, ring->buf_size, ring->size, ring->stride);
		rte_free(ring);
		return -ENOMEM;
	}

	ring->buf = (u8*)tz->addr;
	ring->buf_dma_addr = tz->iova;

	return 0;
}

void mqnic_arm_cq(struct mqnic_cq_ring *ring)
{
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_INTERRUPT_INDEX_REG, ring->eq_index | MQNIC_CPL_QUEUE_ARM_MASK);
}

static void mqnic_active_cpl_queue_registers(struct mqnic_cq_ring *ring)
{
	// deactivate queue
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_ACTIVE_LOG_SIZE_REG, 0);
    // set base address
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_BASE_ADDR_REG+0, ring->buf_dma_addr);
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_BASE_ADDR_REG+4, ring->buf_dma_addr >> 32);
	// set interrupt index
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_INTERRUPT_INDEX_REG, ring->eq_index);
	// set pointers
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_HEAD_PTR_REG, ring->head_ptr & ring->hw_ptr_mask);
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_TAIL_PTR_REG, ring->tail_ptr & ring->hw_ptr_mask);
	// set size and activate queue
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_ACTIVE_LOG_SIZE_REG, ilog2(ring->size) | MQNIC_CPL_QUEUE_ACTIVE_MASK);

	ring->active = 1;
}

static void
mqnic_tx_cpl_queue_active(struct mqnic_if *interface)
{
	struct mqnic_cq_ring *ring;
	uint32_t i;

	PMD_INIT_LOG(DEBUG, "mqnic_tx_cpl_queue_active");

	for (i = 0; i < interface->tx_cpl_queue_count; i++){
		ring = interface->tx_cpl_ring[i];
		if (ring == NULL) {
			PMD_INIT_LOG(ERR, "invalid tx cpl ring buffer, i = %d.", i);
			return;
		}

		ring->eq_ring = interface->event_ring[i % interface->event_queue_count];
		ring->eq_index = ring->eq_ring->index;

		mqnic_active_cpl_queue_registers(ring);
		mqnic_arm_cq(ring);
	}

	MQNIC_WRITE_FLUSH(interface);
	return;
}

static void
mqnic_rx_cpl_queue_active(struct mqnic_if *interface)
{
	struct mqnic_cq_ring *ring;
	uint32_t i;

	PMD_INIT_LOG(DEBUG, "mqnic_rx_cpl_queue_active");

	for (i = 0; i < interface->rx_cpl_queue_count; i++){
		ring = interface->rx_cpl_ring[i];
		if (ring == NULL) {
			PMD_INIT_LOG(ERR, "invalid rx cpl ring buffer, i = %d.", i);
			return;
		}

		ring->eq_ring = interface->event_ring[i % interface->event_queue_count];
		ring->eq_index = ring->eq_ring->index;

		mqnic_active_cpl_queue_registers(ring);
		mqnic_arm_cq(ring);
	}

	MQNIC_WRITE_FLUSH(interface);
	return;
}


static void
mqnic_init_cpl_queue_registers(struct mqnic_cq_ring *ring)
{
	// deactivate queue
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_ACTIVE_LOG_SIZE_REG, 0);
	// set base address
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_BASE_ADDR_REG+0, ring->buf_dma_addr);
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_BASE_ADDR_REG+4, ring->buf_dma_addr >> 32);
	// set interrupt index
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_INTERRUPT_INDEX_REG, 0);
	// set pointers
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_HEAD_PTR_REG, ring->head_ptr & ring->hw_ptr_mask);
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_TAIL_PTR_REG, ring->tail_ptr & ring->hw_ptr_mask);
	// set size
	MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_ACTIVE_LOG_SIZE_REG, ilog2(ring->size));
}

static int
mqnic_tx_cpl_queue_create(struct mqnic_if *interface)
{
	struct mqnic_cq_ring *ring;
	uint32_t i;
	struct mqnic_hw *hw = interface->hw;
	
	PMD_INIT_LOG(DEBUG, "mqnic_tx_cpl_queue_create");

	for (i = 0; i < interface->tx_cpl_queue_count; i++){
		// Release existing buffers if have
		if (interface->tx_cpl_ring[i] != NULL) {
			PMD_INIT_LOG(DEBUG, "release tx cpl ring %d", i);
			mqnic_cpl_queue_release(interface->tx_cpl_ring[i]);
			interface->tx_cpl_ring[i] = NULL;
		}

		/* allocate the completion queue data structure */
		ring = rte_zmalloc("ethdev tx cpl queue", sizeof(struct mqnic_cq_ring),
							RTE_CACHE_LINE_SIZE);
		if (ring == NULL){
			PMD_INIT_LOG(ERR, "failed to alloc tx cpl queue");
			return -ENOMEM;
		}

		// Create completion queue
		_create_cpl_queue(ring, interface, i);

		interface->tx_cpl_ring[i] = ring;
	}

	MQNIC_WRITE_FLUSH(interface);
	return 0;
}


static int
mqnic_tx_cpl_queue_alloc(struct mqnic_if *interface, int socket_id)
{
	struct mqnic_cq_ring *ring;
	uint32_t i;
	int ret;
	struct mqnic_hw *hw = interface->hw;
	struct rte_eth_dev *dev = hw->dev;
	
	PMD_INIT_LOG(DEBUG, "mqnic_tx_cpl_queue_create");

	for (i = 0; i < interface->tx_cpl_queue_count; i++){
		// Release existing buffers if have
		if (interface->tx_cpl_ring[i]->buf != NULL) {
			return -EINVAL;
		}

		ring = interface->tx_cpl_ring[i];

		// Allocate completion queue buffer
		if ((ret = _alloc_cpl_queue(ring, dev, i, socket_id)))
			return ret;

		// Write settings into hardware
		mqnic_init_cpl_queue_registers(ring);

		interface->tx_cpl_ring[i] = ring;
	}

	MQNIC_WRITE_FLUSH(interface);
	return 0;
}

static void
mqnic_tx_cpl_queue_destroy(struct mqnic_if *interface)
{
	//const struct rte_memzone *tz;
	struct mqnic_cq_ring *ring;
	uint32_t i;
	struct rte_eth_dev *dev = interface->hw->dev;
	
	PMD_INIT_LOG(DEBUG, "mqnic_tx_cpl_queue_destroy");

	for (i = 0; i < interface->tx_cpl_queue_count; i++){

		if (interface->tx_cpl_ring[i] != NULL) {
			ring = interface->tx_cpl_ring[i];
			// deactivate queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_ACTIVE_LOG_SIZE_REG, ilog2(ring->size));
    		// disarm queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_INTERRUPT_INDEX_REG, ring->eq_index);
			MQNIC_WRITE_FLUSH(interface);
			PMD_INIT_LOG(DEBUG, "release tx cpl ring %d", i);
			mqnic_cpl_queue_release(ring);
			interface->tx_cpl_ring[i] = NULL;
		}

		rte_eth_dma_zone_free(dev, "tx_cq_ring", i);
	}

	return;
}

static void
mqnic_tx_cpl_queue_deactivate(struct mqnic_if *interface)
{
	//const struct rte_memzone *tz;
	struct mqnic_cq_ring *ring;
	uint32_t i;
	
	PMD_INIT_LOG(DEBUG, "mqnic_tx_cpl_queue_deactivate");

	for (i = 0; i < interface->tx_cpl_queue_count; i++){
		if (interface->tx_cpl_ring[i] != NULL) {
			ring = interface->tx_cpl_ring[i];
			// deactivate queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_ACTIVE_LOG_SIZE_REG, ilog2(ring->size));
    		// disarm queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_INTERRUPT_INDEX_REG, ring->eq_index);
			MQNIC_WRITE_FLUSH(interface);
		}
	}

	return;
}

static int
mqnic_rx_cpl_queue_create(struct mqnic_if *interface)
{
	struct mqnic_cq_ring *ring;
	uint32_t i;
	
	PMD_INIT_LOG(DEBUG, "mqnic_rx_cpl_queue_create");

	for (i = 0; i < interface->rx_cpl_queue_count; i++){
		/* Free memory prior to re-allocation if needed */
		if (interface->rx_cpl_ring[i] != NULL) {
			PMD_INIT_LOG(DEBUG, "release rx cpl ring %d", i);
			mqnic_cpl_queue_release(interface->rx_cpl_ring[i]);
			interface->rx_cpl_ring[i] = NULL;
		}

		/* allocate the event queue data structure */
		ring = rte_zmalloc("ethdev rx cpl queue", sizeof(struct mqnic_cq_ring),
							RTE_CACHE_LINE_SIZE);
		if (ring == NULL){
			PMD_INIT_LOG(ERR, "failed to alloc rx cpl queue");
			return -ENOMEM;
		}

		// Create completion queue
		_create_cpl_queue(ring, interface, i);

		interface->rx_cpl_ring[i] = ring;
	}

	MQNIC_WRITE_FLUSH(interface);
	return 0;
}

static int
mqnic_rx_cpl_queue_alloc(struct mqnic_if *interface, int socket_id)
{
	struct mqnic_cq_ring *ring;
	uint32_t i;
	int ret;
	struct mqnic_hw *hw = interface->hw;
	struct rte_eth_dev *dev = hw->dev;
	
	PMD_INIT_LOG(DEBUG, "mqnic_rx_cpl_queue_create");

	for (i = 0; i < interface->rx_cpl_queue_count; i++){
		/* Free memory prior to re-allocation if needed */
		if (interface->rx_cpl_ring[i]->buf != NULL) {
			return -EINVAL;
		}

		/* allocate the event queue data structure */
		ring = interface->rx_cpl_ring[i];

		// Allocate completion queue buffer
		if ((ret = _alloc_cpl_queue(ring, dev, i, socket_id)))
			return ret;

		// Write settings into hardware
		mqnic_init_cpl_queue_registers(ring);
	}

	MQNIC_WRITE_FLUSH(interface);
	return 0;
}

static void
mqnic_rx_cpl_queue_destroy(struct mqnic_if *interface)
{
	//const struct rte_memzone *tz;
	struct mqnic_cq_ring *ring;
	uint32_t i;
	struct rte_eth_dev *dev = interface->hw->dev;
	
	PMD_INIT_LOG(DEBUG, "mqnic_rx_cpl_queue_destroy");

	for (i = 0; i < interface->rx_cpl_queue_count; i++){
		/* Free memory prior to re-allocation if needed */
		if (interface->rx_cpl_ring[i] != NULL) {
			ring = interface->rx_cpl_ring[i];
			// deactivate queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_ACTIVE_LOG_SIZE_REG, ilog2(ring->size));
			// disarm queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_INTERRUPT_INDEX_REG, ring->eq_index);
			MQNIC_WRITE_FLUSH(interface);
			PMD_INIT_LOG(DEBUG, "release rx cpl ring %d", i);
			mqnic_cpl_queue_release(ring);
			interface->rx_cpl_ring[i] = NULL;
		}

		rte_eth_dma_zone_free(dev, "rx_cq_ring", i);
	}

	return;
}

static void
mqnic_rx_cpl_queue_deactivate(struct mqnic_if *interface)
{
	//const struct rte_memzone *tz;
	struct mqnic_cq_ring *ring;
	uint32_t i;
	
	PMD_INIT_LOG(DEBUG, "mqnic_rx_cpl_queue_deactivate");

	for (i = 0; i < interface->rx_cpl_queue_count; i++){
		/* Free memory prior to re-allocation if needed */
		if (interface->rx_cpl_ring[i] != NULL) {
			ring = interface->rx_cpl_ring[i];
			// deactivate queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_ACTIVE_LOG_SIZE_REG, ilog2(ring->size));
    		// disarm queue
			MQNIC_DIRECT_WRITE_REG(ring->hw_addr, MQNIC_CPL_QUEUE_INTERRUPT_INDEX_REG, ring->eq_index);
			MQNIC_WRITE_FLUSH(interface);
		}
	}

	return;
}

static u32
mqnic_determine_desc_block_size(struct mqnic_if *interface)
{
	u32 desc_block_size = 0;
	MQNIC_DIRECT_WRITE_REG(interface->hw_addr,
		interface->tx_queue_offset + MQNIC_QUEUE_ACTIVE_LOG_SIZE_REG, 0xf << 8);
	interface->max_desc_block_size = 1 << ((MQNIC_DIRECT_READ_REG(interface->hw_addr,
		interface->tx_queue_offset + MQNIC_QUEUE_ACTIVE_LOG_SIZE_REG) >> 8) & 0xf);
	MQNIC_DIRECT_WRITE_REG(interface->hw_addr,
		interface->tx_queue_offset + MQNIC_QUEUE_ACTIVE_LOG_SIZE_REG, 0);

	PMD_INIT_LOG(INFO, "Max desc block size: %d", interface->max_desc_block_size);

	interface->max_desc_block_size = interface->max_desc_block_size < MQNIC_MAX_FRAGS ? interface->max_desc_block_size : MQNIC_MAX_FRAGS;

	desc_block_size = interface->max_desc_block_size < 4 ? interface->max_desc_block_size : 4;
	return desc_block_size;
}

/*static void mqnic_port_set_rss_mask(struct mqnic_port *port, u32 rss_mask)*/
/*{*/
	/*MQNIC_DIRECT_WRITE_REG(port->hw_addr, MQNIC_PORT_REG_RSS_MASK, rss_mask);*/
/*}*/

static void mqnic_deactivate_scheduler(struct mqnic_sched *sched)
{
	// disable schedulers
	MQNIC_DIRECT_WRITE_REG(sched->rb->regs, MQNIC_RB_SCHED_RR_REG_CTRL, 0);
}

/*static int mqnic_activate_first_port(struct rte_eth_dev *dev)*/
/*{*/
	/*uint32_t k;*/
	/*struct mqnic_priv *priv =*/
		/*MQNIC_DEV_PRIVATE_TO_PRIV(dev->data->dev_private);*/
	/*struct mqnic_port *port = priv->ports[0];*/

	/*// enable schedulers*/
	/*MQNIC_DIRECT_WRITE_REG(port->hw_addr, MQNIC_PORT_REG, 0xffffffff);*/

	/*// enable queues*/
	/*for (k = 0; k < port->tx_queue_count; k++)*/
	/*{*/
		/*MQNIC_DIRECT_WRITE_REG(port->hw_addr, port->sched_offset+k*4, 3);*/
	/*}*/
	/*MQNIC_WRITE_FLUSH(priv);*/

	/*return 0;*/
/*}*/

static void
mqnic_set_interface_mtu(struct mqnic_if *interface, uint32_t mtu)
{
	MQNIC_DIRECT_WRITE_REG(interface->if_ctrl_rb, MQNIC_RB_IF_CTRL_REG_MAX_RX_MTU, mtu+ETH_HLEN);
	MQNIC_DIRECT_WRITE_REG(interface->if_ctrl_rb, MQNIC_RB_IF_CTRL_REG_MAX_TX_MTU, mtu+ETH_HLEN);
}


static int mqnic_single_port_create(struct mqnic_if *interface, int i) {
	int ret=0;
	u32 offset;
	struct mqnic_reg_block *port_rb;
	struct mqnic_reg_block *rb;

	/* allocate the event queue data structure */
	struct mqnic_port *port = rte_zmalloc("ethdev port", sizeof(struct mqnic_port),
						RTE_CACHE_LINE_SIZE);
	if (port == NULL){
		PMD_INIT_LOG(ERR, "Failed to alloc port");
		ret = -ENOMEM;
		goto fail;
	}

	// Get register block for port i
	port_rb = mqnic_find_reg_block(interface->rb_list, MQNIC_RB_PORT_TYPE, MQNIC_RB_PORT_VER, i);
	if (!port_rb) {
		PMD_INIT_LOG(ERR, "Port %d not found", i);
		ret = -EIO;
		goto fail;
	}

	port->index = i;
	port->interface = interface;
	port->port_rb = port_rb;

	// Emumerate register block list for port i
	offset = MQNIC_DIRECT_READ_REG(port_rb->regs, MQNIC_RB_SCHED_BLOCK_REG_OFFSET);
	port->rb_list = mqnic_enumerate_reg_block_list(interface->hw_addr,
			offset, interface->hw_regs_size - offset);
	if (port->rb_list == NULL){
		PMD_INIT_LOG(ERR, "Failed to enumerate blocks");
		ret = -EIO;
		goto fail;
	}

	PMD_INIT_LOG(INFO, "Port-level register blocks:");
	for (rb = port->rb_list; rb->regs; rb++)
		PMD_INIT_LOG(INFO, " type 0x%08x (v %d.%d.%d.%d)", rb->type, rb->version >> 24,
			(rb->version >> 16) & 0xff, (rb->version >> 8) & 0xff, rb->version & 0xff);

	// Get control register blocks
	port->port_ctrl_rb = mqnic_find_reg_block(port->rb_list, MQNIC_RB_PORT_CTRL_TYPE, MQNIC_RB_PORT_CTRL_VER, 0);
	if (!port->port_ctrl_rb) {
		PMD_INIT_LOG(ERR, "Port control register block not found");
		ret = -EIO;
		goto fail;
	}

	// read ID registers
	port->port_features = MQNIC_DIRECT_READ_REG(port->port_ctrl_rb->regs, MQNIC_RB_PORT_CTRL_REG_FEATURES);
	PMD_INIT_LOG(INFO, "Port features: 0x%08x", port->port_features);
	PMD_INIT_LOG(INFO, "Port TX status: 0x%08x", mqnic_port_get_tx_status(port));
	PMD_INIT_LOG(INFO, "Port RX status: 0x%08x", mqnic_port_get_rx_status(port));

	/*port->port_mtu = MQNIC_DIRECT_READ_REG(port->hw_addr, MQNIC_PORT_REG_PORT_MTU);*/
	/*PMD_INIT_LOG(INFO, "Port MTU: %d", port->port_mtu);*/
	/*port->sched_count = MQNIC_DIRECT_READ_REG(port->hw_addr, MQNIC_PORT_REG_SCHED_COUNT);*/
	/*PMD_INIT_LOG(INFO, "Scheduler count: %d", port->sched_count);*/
	/*port->sched_offset = MQNIC_DIRECT_READ_REG(port->hw_addr, MQNIC_PORT_REG_SCHED_OFFSET);*/
	/*PMD_INIT_LOG(INFO, "Scheduler offset: 0x%08x", port->sched_offset);*/
	/*port->sched_stride = MQNIC_DIRECT_READ_REG(port->hw_addr, MQNIC_PORT_REG_SCHED_STRIDE);*/
	/*PMD_INIT_LOG(INFO, "Scheduler stride: 0x%08x", port->sched_stride);*/
	/*port->sched_type = MQNIC_DIRECT_READ_REG(port->hw_addr, MQNIC_PORT_REG_SCHED_TYPE);*/
	/*PMD_INIT_LOG(INFO, "Scheduler type: 0x%08x", port->sched_type);*/

	/*mqnic_deactivate_port(port);*/
	/*mqnic_port_set_rss_mask(port, 0xffffffff);*/
	interface->port[i] = port;
fail:
	mqnic_single_port_destroy(port);
	return ret;
}

void mqnic_single_port_destroy(struct mqnic_port *port) {
	/*mqnic_deactivate_port(port);*/
	mqnic_free_reg_block_list(port->rb_list);
	rte_free(port);
}

int mqnic_all_ports_create(struct mqnic_if *interface)
{
	uint32_t i;
	int ret;
	PMD_INIT_LOG(DEBUG, "eth_mqnic_all_ports_create");

	for (i = 0; i < interface->port_count; i++){
		mqnic_single_port_create(interface, i);
	}
	MQNIC_WRITE_FLUSH(interface);

fail:
	mqnic_all_ports_destroy(interface);
	return ret;
}

void mqnic_all_ports_destroy(struct mqnic_if *interface)
{
	struct mqnic_port *port;
	uint32_t i;

	PMD_INIT_LOG(DEBUG, "mqnic_all_ports_destroy");

	for (i = 0; i < interface->port_count; i++){
		if (interface->port[i] != NULL) {
			PMD_INIT_LOG(DEBUG, "release port %d", i);
			mqnic_single_port_destroy(interface->port[i]);
			interface->port[i] = NULL;
		}
	}

	return;
}

static void
mqnic_all_port_deactivate(struct mqnic_if *interface)
{
	uint32_t i;
	struct mqnic_port *port;

	PMD_INIT_LOG(DEBUG, "mqnic_all_port_deactivate");

	for (i = 0; i < interface->port_count; i++){
		if (interface->port[i] != NULL) {
			port = interface->port[i];
			if (port->rb_list) {
				mqnic_free_reg_block_list(port->rb_list);
			}
			interface->port[i] = NULL;
			MQNIC_WRITE_FLUSH(interface);
		}
	}

	return;
}

int mqnic_sched_block_create(struct mqnic_if *interface) {
	int ret = 0;
	int i;
	u32 offset;
	struct mqnic_reg_block *sched_block_rb;
	struct mqnic_sched_block *block;

	for (i=0; i<interface->sched_block_count; i++) {
		sched_block_rb = mqnic_find_reg_block(interface->rb_list,
			MQNIC_RB_SCHED_BLOCK_TYPE, MQNIC_RB_SCHED_BLOCK_VER, i);
		if (!sched_block_rb) {
			ret = -EIO;
			PMD_INIT_LOG(ERR, "Scheduler block index %d not found", i);
		}

		block = rte_zmalloc("scheduler block", sizeof(struct mqnic_sched_block), MQNIC_ALIGN);
		if (!block) {
			return -ENOMEM;
		}

		block->interface = interface;

		block->index = i;
		block->tx_queue_count = interface->tx_queue_count;
		block->block_rb = sched_block_rb;
		offset = MQNIC_DIRECT_READ_REG(sched_block_rb->regs, MQNIC_RB_SCHED_BLOCK_REG_OFFSET);

		block->rb_list = mqnic_enumerate_reg_block_list(interface->hw_addr, offset, interface->hw_regs_size - offset);
		if (!block->rb_list) {
			ret = -EIO;
			PMD_INIT_LOG(ERR, "Failed to enumerate blocks");
			goto fail;
		}

		PMD_INIT_LOG(INFO, "Scheduler block-level register blocks:");
		for (struct mqnic_reg_block *rb = block->rb_list; rb->regs; rb++)
			PMD_INIT_LOG(INFO, " type 0x%08x (v %d.%d.%d.%d)", rb->type, rb->version >> 24,
			(rb->version >> 16) & 0xff, (rb->version >> 8) & 0xff, rb->version & 0xff);

		block->sched_count = 0;
		for (struct mqnic_reg_block *rb = block->rb_list; rb->regs; rb++) {
			if (rb->type == MQNIC_RB_SCHED_RR_TYPE && rb->version == MQNIC_RB_SCHED_RR_VER) {
				ret = mqnic_scheduler_create(block, &block->sched[block->sched_count],
						block->sched_count, rb);

				if (ret)
					goto fail;

				block->sched_count++;
			}
		}
	}

	PMD_INIT_LOG(INFO, "Scheduler count: %d", block->sched_count);

	mqnic_deactivate_sched_block(block);
	return 0;

fail:
	mqnic_destroy_sched_block(&block);
	return ret;
}

void mqnic_destroy_sched_block(struct mqnic_sched_block **block_p)
{
	int i;
	struct mqnic_sched_block *block = *block_p;

	mqnic_deactivate_sched_block(block);

	for (i = 0; i < block->sched_count; i++)
		if (block->sched[i])
			mqnic_destroy_scheduler(&block->sched[i]);

	if (block->rb_list)
		mqnic_free_reg_block_list(block->rb_list);

	*block_p = NULL;
	rte_free(block);
}

void mqnic_deactivate_sched_block(struct mqnic_sched_block *block)
{
	int i;

	// disable schedulers
	for (i = 0; i < block->sched_count; i++)
		if (block->sched[i])
			mqnic_scheduler_disable(block->sched[i]);
}

int mqnic_scheduler_create(struct mqnic_sched_block *block, struct mqnic_sched **sched_p, int idx, struct mqnic_reg_block *rb) {
	struct mqnic_if *interface = block->interface;
	struct mqnic_sched *sched = rte_zmalloc("mqnic scheduler", sizeof(struct mqnic_sched), MQNIC_ALIGN);
	if (!sched) {
		return -ENOMEM;
	}

	sched->interface = interface;
	sched->sched_block = block;

	sched->index = idx;
	sched->rb = rb;
	sched->type = rb->type;

	sched->offset = MQNIC_DIRECT_READ_REG(rb->regs, MQNIC_RB_SCHED_RR_REG_OFFSET);
	sched->channel_count = MQNIC_DIRECT_READ_REG(rb->regs, MQNIC_RB_SCHED_RR_REG_CH_COUNT);
	sched->channel_stride = MQNIC_DIRECT_READ_REG(rb->regs, MQNIC_RB_SCHED_RR_REG_CH_STRIDE);

	sched->hw_addr = block->interface->hw_addr + sched->offset;

	PMD_INIT_LOG(INFO, "Scheduler type: 0x%08x", sched->type);
	PMD_INIT_LOG(INFO, "Scheduler offset: 0x%08x", sched->offset);
	PMD_INIT_LOG(INFO, "Scheduler channel count: %d", sched->channel_count);
	PMD_INIT_LOG(INFO, "Scheduler channel stride: 0x%08x", sched->channel_stride);

	mqnic_scheduler_disable(sched);

	*sched_p = sched;
}

void mqnic_destroy_scheduler(struct mqnic_sched **sched_ptr)
{
	struct mqnic_sched *sched = *sched_ptr;
	*sched_ptr = NULL;

	mqnic_scheduler_disable(sched);

	rte_free(sched);
}

void mqnic_scheduler_disable(struct mqnic_sched *sched)
{
	MQNIC_DIRECT_WRITE_REG(sched->rb->regs, MQNIC_RB_SCHED_RR_REG_CTRL, 0);
}

int mqnic_create_if(struct rte_eth_dev *dev, int idx) {
	int ret = 0;
	u32 i = 0;
	u32 desc_block_size;
	struct mqnic_if *interface;
	struct mqnic_reg_block *rb;
	struct mqnic_hw *hw = MQNIC_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	interface = rte_zmalloc("mqnic interface", sizeof(struct mqnic_if), 0);
	if (!interface)
		return -ENOMEM;

	interface->index = idx;
	interface->hw_regs_size = hw->if_stride;
	interface->hw_addr = hw->hw_addr + hw->if_offset + idx * hw->if_stride;
	interface->csr_hw_addr = interface->hw_addr + hw->if_csr_offset;

	// Enumerate registers
	interface->rb_list = mqnic_enumerate_reg_block_list(interface->hw_addr, hw->if_csr_offset, interface->hw_regs_size);
	if (!interface->rb_list) {
		ret = -EIO;
		PMD_INIT_LOG(ERR, "Failed to enumerate blocks from 0x%p", interface->hw_addr);
		goto fail;
	}
	PMD_INIT_LOG(INFO, "Interface-level register blocks:");
	for (rb = interface->rb_list; rb->regs; rb++)
		PMD_INIT_LOG(INFO, "\ttype 0x%08x (v %d.%d.%d.%d)", rb->type, rb->version >> 24,
				(rb->version >> 16) & 0xff, (rb->version >> 8) & 0xff, rb->version & 0xff);
	
	// Read interface features
	interface->if_ctrl_rb = mqnic_find_reg_block(interface->rb_list, MQNIC_RB_IF_CTRL_TYPE, MQNIC_RB_IF_CTRL_VER, 0);
	if (!interface->if_ctrl_rb) {
		ret = -EIO;
		PMD_INIT_LOG(ERR, "Interface control block not found");
		goto fail;
	}

	interface->if_features = MQNIC_DIRECT_READ_REG(interface->if_ctrl_rb->regs, MQNIC_RB_IF_CTRL_REG_FEATURES);
	interface->port_count = MQNIC_DIRECT_READ_REG(interface->if_ctrl_rb->regs, MQNIC_RB_IF_CTRL_REG_PORT_COUNT);
	interface->sched_block_count = MQNIC_DIRECT_READ_REG(interface->if_ctrl_rb->regs, MQNIC_RB_IF_CTRL_REG_SCHED_COUNT);
	interface->max_tx_mtu = MQNIC_DIRECT_READ_REG(interface->if_ctrl_rb->regs, MQNIC_RB_IF_CTRL_REG_MAX_TX_MTU);
	interface->max_rx_mtu = MQNIC_DIRECT_READ_REG(interface->if_ctrl_rb->regs, MQNIC_RB_IF_CTRL_REG_MAX_RX_MTU);

	PMD_INIT_LOG(INFO, "IF features: 0x%08x", interface->if_features);
	PMD_INIT_LOG(INFO, "Port count: %d", interface->port_count);
	PMD_INIT_LOG(INFO, "Scheduler block count: %d", interface->sched_block_count);
	PMD_INIT_LOG(INFO, "Max TX MTU: %d", interface->max_tx_mtu);
	PMD_INIT_LOG(INFO, "Max RX MTU: %d", interface->max_rx_mtu);

	// Read event queue
	interface->event_queue_rb = mqnic_find_reg_block(interface->rb_list, MQNIC_RB_EVENT_QM_TYPE, MQNIC_RB_EVENT_QM_VER, 0);
	if (!interface->event_queue_rb) {
		ret = -EIO;
		PMD_INIT_LOG(ERR, "Event queue block not found");
		goto fail;
	}

	interface->event_queue_count = MQNIC_DIRECT_READ_REG(interface->event_queue_rb->regs, MQNIC_RB_EVENT_QM_REG_COUNT);
	interface->event_queue_offset = MQNIC_DIRECT_READ_REG(interface->event_queue_rb->regs, MQNIC_RB_EVENT_QM_REG_OFFSET);
	interface->event_queue_stride = MQNIC_DIRECT_READ_REG(interface->event_queue_rb->regs, MQNIC_RB_EVENT_QM_REG_STRIDE);

	PMD_INIT_LOG(INFO, "Event queue offset: 0x%08x", interface->event_queue_offset);
	PMD_INIT_LOG(INFO, "Event queue count: %d", interface->event_queue_count);
	PMD_INIT_LOG(INFO, "Event queue stride: 0x%08x", interface->event_queue_stride);

	if (interface->event_queue_count > MQNIC_MAX_EVENT_RINGS)
		interface->event_queue_count = MQNIC_MAX_EVENT_RINGS;

	// Read transmit queue
	interface->tx_queue_rb = mqnic_find_reg_block(interface->rb_list, MQNIC_RB_TX_QM_TYPE, MQNIC_RB_TX_QM_VER, 0);
	if (!interface->tx_queue_rb) {
		ret = -EIO;
		PMD_INIT_LOG(ERR, "Transmit queue block not found");
		goto fail;
	}

	interface->tx_queue_offset = MQNIC_DIRECT_READ_REG(interface->tx_queue_rb->regs, MQNIC_RB_TX_QM_REG_OFFSET);
	interface->tx_queue_count = MQNIC_DIRECT_READ_REG(interface->tx_queue_rb->regs, MQNIC_RB_TX_QM_REG_COUNT);
	interface->tx_queue_stride = MQNIC_DIRECT_READ_REG(interface->tx_queue_rb->regs, MQNIC_RB_TX_QM_REG_STRIDE);

	PMD_INIT_LOG(INFO, "TX queue offset: 0x%08x", interface->tx_queue_offset);
	PMD_INIT_LOG(INFO, "TX queue count: %d", interface->tx_queue_count);
	PMD_INIT_LOG(INFO, "TX queue stride: 0x%08x", interface->tx_queue_stride);

	if (interface->tx_queue_count > MQNIC_MAX_TX_RINGS)
		interface->tx_queue_count = MQNIC_MAX_TX_RINGS;

	// Read transmit completion queue
	interface->tx_cpl_queue_rb = mqnic_find_reg_block(interface->rb_list, MQNIC_RB_TX_CQM_TYPE, MQNIC_RB_TX_CQM_VER, 0);
	if (!interface->tx_cpl_queue_rb) {
		ret = -EIO;
		PMD_INIT_LOG(ERR, "TX completion queue block not found");
		goto fail;
	}

	interface->tx_cpl_queue_offset = MQNIC_DIRECT_READ_REG(interface->tx_cpl_queue_rb->regs,
			MQNIC_RB_TX_CQM_REG_OFFSET);
	interface->tx_cpl_queue_count = MQNIC_DIRECT_READ_REG(interface->tx_cpl_queue_rb->regs,
			MQNIC_RB_TX_CQM_REG_COUNT);
	interface->tx_cpl_queue_stride = MQNIC_DIRECT_READ_REG(interface->tx_cpl_queue_rb->regs,
			MQNIC_RB_TX_CQM_REG_STRIDE);

	PMD_INIT_LOG(INFO, "TX completion queue offset: 0x%08x", interface->tx_cpl_queue_offset);
	PMD_INIT_LOG(INFO, "TX completion queue count: %d", interface->tx_cpl_queue_count);
	PMD_INIT_LOG(INFO, "TX completion queue stride: 0x%08x", interface->tx_cpl_queue_stride);

	if (interface->tx_cpl_queue_count > MQNIC_MAX_TX_CPL_RINGS)
		interface->tx_cpl_queue_count = MQNIC_MAX_TX_CPL_RINGS;

	// Read receive queue
	interface->rx_queue_rb = mqnic_find_reg_block(interface->rb_list, MQNIC_RB_RX_QM_TYPE,
			MQNIC_RB_RX_QM_VER, 0);
	if (!interface->rx_queue_rb) {
		ret = -EIO;
		PMD_INIT_LOG(ERR, "Transmit queue block not found");
		goto fail;
	}

	interface->rx_queue_offset = MQNIC_DIRECT_READ_REG(interface->rx_queue_rb->regs,
			MQNIC_RB_RX_QM_REG_OFFSET);
	interface->rx_queue_count = MQNIC_DIRECT_READ_REG(interface->rx_queue_rb->regs,
			MQNIC_RB_RX_QM_REG_COUNT);
	interface->rx_queue_stride = MQNIC_DIRECT_READ_REG(interface->rx_queue_rb->regs,
			MQNIC_RB_RX_QM_REG_STRIDE);

	PMD_INIT_LOG(INFO, "RX queue offset: 0x%08x", interface->rx_queue_offset);
	PMD_INIT_LOG(INFO, "RX queue count: %d", interface->rx_queue_count);
	PMD_INIT_LOG(INFO, "RX queue stride: 0x%08x", interface->rx_queue_stride);

	if (interface->rx_queue_count > MQNIC_MAX_RX_RINGS)
		interface->rx_queue_count = MQNIC_MAX_RX_RINGS;

	// Read receive completion queue
	interface->rx_cpl_queue_rb = mqnic_find_reg_block(interface->rb_list, MQNIC_RB_RX_CQM_TYPE, MQNIC_RB_RX_CQM_VER, 0);
	if (!interface->rx_cpl_queue_rb) {
		ret = -EIO;
		PMD_INIT_LOG(ERR, "RX completion queue block not found");
		goto fail;
	}

	interface->rx_cpl_queue_offset = MQNIC_DIRECT_READ_REG(interface->rx_cpl_queue_rb->regs, MQNIC_RB_RX_CQM_REG_OFFSET);
	interface->rx_cpl_queue_count = MQNIC_DIRECT_READ_REG(interface->rx_cpl_queue_rb->regs, MQNIC_RB_RX_CQM_REG_COUNT);
	interface->rx_cpl_queue_stride = MQNIC_DIRECT_READ_REG(interface->rx_cpl_queue_rb->regs, MQNIC_RB_RX_CQM_REG_STRIDE);

	PMD_INIT_LOG(INFO, "RX completion queue offset: 0x%08x", interface->rx_cpl_queue_offset);
	PMD_INIT_LOG(INFO, "RX completion queue count: %d", interface->rx_cpl_queue_count);
	PMD_INIT_LOG(INFO, "RX completion queue stride: 0x%08x", interface->rx_cpl_queue_stride);

	if (interface->rx_cpl_queue_count > MQNIC_MAX_RX_CPL_RINGS)
		interface->rx_cpl_queue_count = MQNIC_MAX_RX_CPL_RINGS;

	// Read receive queue map
	interface->rx_queue_map_rb = mqnic_find_reg_block(interface->rb_list, MQNIC_RB_RX_QUEUE_MAP_TYPE, MQNIC_RB_RX_QUEUE_MAP_VER, 0);

	if (!interface->rx_queue_map_rb) {
		ret = -EIO;
		PMD_INIT_LOG(ERR, "RX queue map block not found");
		goto fail;
	}

	for (i = 0; i < interface->port_count; i++) {
		mqnic_interface_set_rx_queue_map_offset(interface, i, 0);
		mqnic_interface_set_rx_queue_map_rss_mask(interface, i, 0);
		mqnic_interface_set_rx_queue_map_app_mask(interface, i, 0);
	}

	// Determine description block size
	desc_block_size = mqnic_determine_desc_block_size(interface);

	// Create rings
	mqnic_all_event_queue_create(interface);
	mqnic_tx_cpl_queue_create(interface);
	mqnic_rx_cpl_queue_create(interface);

	// Create ports
	mqnic_all_ports_create(interface);
	
	// Create schedulers
	mqnic_sched_block_create(interface);

	// Create net device.
	// Currently, there is only one device.
	// TODO: multiple devices
	interface->dev_count = 1;
	interface->eth_dev[0] = hw->dev;
	for (i = 0; i < interface->dev_count; i++) {
		ret = mqnic_ethdev_create(interface, &interface->eth_dev[i], i);
		if (ret)
			goto fail;
	}

	hw->interface[idx] = interface;

fail:
	mqnic_free_reg_block_list(interface->rb_list);
	return ret;
}


int32_t mqnic_get_basic_info_from_hw(struct mqnic_hw *hw)
{
    // Read ID registers
    hw->fpga_id = MQNIC_DIRECT_READ_REG(hw->fw_id_rb, MQNIC_RB_FW_ID_REG_FPGA_ID);
    PMD_INIT_LOG(DEBUG, "FPGA ID: 0x%08x", hw->fpga_id);
    hw->fw_id = MQNIC_DIRECT_READ_REG(hw->fw_id_rb, MQNIC_RB_FW_ID_REG_FW_ID);
    PMD_INIT_LOG(DEBUG, "FW ID: 0x%08x", hw->fw_id);
	if (hw->fw_id == 0xffffffff){
		PMD_INIT_LOG(ERR, "Deivce needs to be reset");
		return MQNIC_ERR_RESET;
	}

    hw->fw_ver = MQNIC_DIRECT_READ_REG(hw->fw_id_rb, MQNIC_RB_FW_ID_REG_FW_VER);
    PMD_INIT_LOG(DEBUG, "FW version: %d.%d.%d.%d", hw->fw_ver >> 24, (hw->fw_ver >> 16) & 0xff,
		(hw->fw_ver >> 8) & 0xff, hw->fw_ver & 0xff);
    hw->board_id = MQNIC_DIRECT_READ_REG(hw->fw_id_rb, MQNIC_RB_FW_ID_REG_BOARD_ID);
    PMD_INIT_LOG(DEBUG, "Board ID: 0x%08x", hw->board_id);
    hw->board_ver = MQNIC_DIRECT_READ_REG(hw->fw_id_rb, MQNIC_RB_FW_ID_REG_BOARD_VER);
    PMD_INIT_LOG(DEBUG, "Board version: %d.%d.%d.%d", hw->board_ver >> 24,
		(hw->board_ver >> 16) & 0xff,
		(hw->board_ver >> 8) & 0xff,
		hw->board_ver & 0xff);
    hw->build_date = MQNIC_DIRECT_READ_REG(hw->fw_id_rb, MQNIC_RB_FW_ID_REG_BUILD_DATE);
    PMD_INIT_LOG(DEBUG, "Build date: %s (raw: %d)", asctime(localtime((time_t *)&(hw->build_date))), hw->build_date);
    hw->git_hash = MQNIC_DIRECT_READ_REG(hw->fw_id_rb, MQNIC_RB_FW_ID_REG_GIT_HASH);
    PMD_INIT_LOG(DEBUG, "Git hash: %08x", hw->git_hash);
    hw->rel_info = MQNIC_DIRECT_READ_REG(hw->fw_id_rb, MQNIC_RB_FW_ID_REG_REL_INFO);
    PMD_INIT_LOG(DEBUG, "Release info: %08x", hw->rel_info);

	return MQNIC_SUCCESS;
}


void mqnic_identify_hardware(struct rte_eth_dev *dev, struct rte_pci_device *pci_dev)
{
	struct mqnic_hw *hw =
		MQNIC_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	hw->vendor_id = pci_dev->id.vendor_id;
	hw->device_id = pci_dev->id.device_id;
	hw->subsystem_vendor_id = pci_dev->id.subsystem_vendor_id;
	hw->subsystem_device_id = pci_dev->id.subsystem_device_id;
}

s32 mqnic_read_mac_addr(struct mqnic_hw *hw)
{
	rte_eth_random_addr(hw->mac.addr);

	/* Set Organizationally Unique Identifier (OUI) prefix */
	hw->mac.addr[0] = 0x00;
	hw->mac.addr[1] = 0xAA;
	hw->mac.addr[2] = 0xBB;

	return MQNIC_SUCCESS;
}

int eth_mqnic_dev_init(struct rte_eth_dev *eth_dev)
{
	int error = 0;
	struct mqnic_reg_block *rb;
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(eth_dev);
	struct mqnic_hw *hw =
		MQNIC_DEV_PRIVATE_TO_HW(eth_dev->data->dev_private);
	struct mqnic_adapter *adapter =
		MQNIC_DEV_PRIVATE(eth_dev->data->dev_private);

	eth_dev->dev_ops = &eth_mqnic_ops;
	eth_dev->rx_pkt_burst = &eth_mqnic_recv_pkts;
	eth_dev->tx_pkt_burst = &eth_mqnic_xmit_pkts;

	/* for secondary processes, we don't initialise any further as primary
	 * has already done this work. Only check we don't need a different
	 * RX function */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY){
		if (eth_dev->data->scattered_rx)
			eth_dev->rx_pkt_burst = &eth_mqnic_recv_scattered_pkts;
		return 0;
	}

	rte_eth_copy_pci_info(eth_dev, pci_dev);
	eth_dev->data->dev_flags |= RTE_ETH_DEV_AUTOFILL_QUEUE_XSTATS;

	hw->hw_addr = (void *)pci_dev->mem_resource[0].addr;
	hw->hw_regs_size = pci_dev->mem_resource[0].len;

	// Enumerate registers
	hw->rb_list = mqnic_enumerate_reg_block_list(hw->hw_addr, 0, hw->hw_regs_size);
	if (!hw->rb_list) {
	    PMD_INIT_LOG(ERR, "Failed to enumerate blocks");
	    error = -EIO;
	    goto err_late;
	}

	PMD_INIT_LOG(INFO, "Device-level register blocks:");
	for (rb = hw->rb_list; rb->regs; rb++) {
	    PMD_INIT_LOG(INFO, "\ttype 0x%08x (v %d.%d.%d.%d)", rb->type, rb->version >> 24,
		    (rb->version >> 16) & 0xff, (rb->version >> 8) & 0xff, rb->version & 0xff);
	}

	mqnic_identify_hardware(eth_dev, pci_dev);

	// Read ID registers
	hw->fw_id_rb = mqnic_find_reg_block(hw->rb_list, MQNIC_RB_FW_ID_TYPE, MQNIC_RB_FW_ID_VER, 0);
	if (!hw->fw_id_rb) {
		error = -EIO;
		PMD_INIT_LOG(ERR, "Error: FW ID block not found");
		goto fail_rb_init;
	}

	// Check basic info
	if (mqnic_get_basic_info_from_hw(hw) != MQNIC_SUCCESS) {
		error = -EIO;
		goto fail_basic_info;
	}

	// Read interface registers
	hw->if_rb = mqnic_find_reg_block(hw->rb_list, MQNIC_RB_IF_TYPE, MQNIC_RB_IF_VER, 0);
	if (!hw->if_rb) {
		error = -EIO;
		PMD_INIT_LOG(ERR, "Error: Interface block not found");
		goto fail_if_rb;
	}

	hw->if_offset = MQNIC_DIRECT_READ_REG(hw->if_rb->regs, MQNIC_RB_IF_REG_OFFSET);
	hw->if_count = MQNIC_DIRECT_READ_REG(hw->if_rb->regs, MQNIC_RB_IF_REG_COUNT);
	hw->if_stride = MQNIC_DIRECT_READ_REG(hw->if_rb->regs, MQNIC_RB_IF_REG_STRIDE);
	hw->if_csr_offset = MQNIC_DIRECT_READ_REG(hw->if_rb->regs, MQNIC_RB_IF_REG_CSR_OFFSET);
	if (hw->if_count > MQNIC_MAX_IF)
		hw->if_count = MQNIC_MAX_IF;

	PMD_INIT_LOG(INFO, "IF offset: 0x%08x", hw->if_offset);
	PMD_INIT_LOG(INFO, "IF count: %d", hw->if_count);
	PMD_INIT_LOG(INFO, "IF stride: 0x%08x", hw->if_stride);
	PMD_INIT_LOG(INFO, "IF CSR offset: 0x%08x", hw->if_csr_offset);

	// check BAR size
	if (hw->if_count * hw->if_stride > hw->hw_regs_size) {
		error = -EIO;
		PMD_INIT_LOG(ERR, "Invalid BAR configuration (%d IF * 0x%x > 0x%llx)",
				hw->if_count, hw->if_stride, hw->hw_regs_size);
		goto fail_bar_size;
	}

	for (int i = 0; i < hw->if_count; i++) {
		PMD_INIT_LOG(INFO, "Creating interface %d", i);
		/*error = eth_mqnic_get_if_hw_info(eth_dev);*/
		error = mqnic_create_if(eth_dev, i);
		if (error) {
			PMD_INIT_LOG(ERR, "Failed to create interface %d", i);
			goto fail_create_if;
		}
	}


	/* Read the permanent MAC address out of the EEPROM */
	if (mqnic_read_mac_addr(hw) != 0) {
		PMD_INIT_LOG(ERR, "EEPROM error while reading MAC address");
		error = -EIO;
		goto err_late;
	}

	/* Allocate memory for storing MAC addresses */
	eth_dev->data->mac_addrs = rte_zmalloc("mqnic",
		RTE_ETHER_ADDR_LEN, 0);
	if (eth_dev->data->mac_addrs == NULL) {
		PMD_INIT_LOG(ERR, "Failed to allocate %d bytes needed to "
						"store MAC addresses",
				RTE_ETHER_ADDR_LEN);
		error = -ENOMEM;
		goto err_late;
	}

	/* Copy the permanent MAC address */
	rte_ether_addr_copy((struct rte_ether_addr *)hw->mac.addr,
			&eth_dev->data->mac_addrs[0]);

	adapter->stopped = 0;

	PMD_INIT_LOG(DEBUG, "port_id %d vendorID=0x%x deviceID=0x%x",
		     eth_dev->data->port_id, pci_dev->id.vendor_id,
		     pci_dev->id.device_id);

	return 0;

fail_create_if:
fail_bar_size:
fail_if_rb:
fail_basic_info:
fail_rb_init:
	mqnic_free_reg_block_list(hw->rb_list);

err_late:
	return error;
}

int eth_mqnic_dev_uninit(struct rte_eth_dev *eth_dev)
{
	PMD_INIT_FUNC_TRACE();

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	eth_mqnic_close(eth_dev);

	return 0;
}

int eth_mqnic_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
	struct rte_pci_device *pci_dev)
{
	return rte_eth_dev_pci_generic_probe(pci_dev,
		sizeof(struct mqnic_adapter), eth_mqnic_dev_init);
}

int eth_mqnic_pci_remove(struct rte_pci_device *pci_dev)
{
	return rte_eth_dev_pci_generic_remove(pci_dev, eth_mqnic_dev_uninit);
}

static struct rte_pci_driver rte_mqnic_pmd = {
	.id_table = pci_id_mqnic_map,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING,
	.probe = eth_mqnic_pci_probe,
	.remove = eth_mqnic_pci_remove,
};

static int mqnic_check_mq_mode(struct rte_eth_dev *dev)
{
	RTE_SET_USED(dev);
	return 0;
}

static int
eth_mqnic_configure(struct rte_eth_dev *dev)
{
	int ret;

	PMD_INIT_FUNC_TRACE();

	/* multipe queue mode checking */
	ret  = mqnic_check_mq_mode(dev);
	if (ret != 0) {
		PMD_INIT_LOG(ERR, "mqnic_check_mq_mode fails with %d.", ret);
		return ret;
	}

	PMD_INIT_FUNC_TRACE();

	return 0;
}

static int eth_mqnic_start(struct rte_eth_dev *dev)
{
	struct mqnic_adapter *adapter =
		MQNIC_DEV_PRIVATE(dev->data->dev_private);
	struct mqnic_hw *hw = &adapter->hw;
	struct mqnic_if *interface;
	int idx;
	int ret = 0;

	PMD_INIT_FUNC_TRACE();
	adapter->stopped = 0;

	for (idx=0; idx<hw->if_count; idx++) {
		interface = hw->interface[idx];
		PMD_INIT_LOG(INFO, "Starting interface %d", idx);

		mqnic_all_event_queue_active(interface);
		mqnic_rx_cpl_queue_active(interface);

		/* This can fail when allocating mbufs for descriptor rings */
		ret = eth_mqnic_rx_init(dev);
		if (ret) {
			PMD_INIT_LOG(ERR, "Unable to initialize RX hardware");
			mqnic_dev_clear_queues(dev);
			return ret;
		}

		mqnic_tx_cpl_queue_active(interface);
		eth_mqnic_tx_init(dev);
	}

	mqnic_set_interface_mtu(dev, 1500);
	/*mqnic_activate_first_port(dev);*/
	/*interface->port_up = true;*/

	eth_mqnic_link_update(dev, 0);

	PMD_INIT_LOG(DEBUG, "<<");

	return 0;
}

/*********************************************************************
 *
 *  This routine disables all traffic on the adapter by issuing a
 *  global reset on the MAC.
 *
 **********************************************************************/
static int
eth_mqnic_stop(struct rte_eth_dev *dev)
{
	int idx;
	struct rte_eth_link link;
	struct mqnic_adapter *adapter =
		MQNIC_DEV_PRIVATE(dev->data->dev_private);
	struct mqnic_hw *hw = &adapter->hw;
	struct mqnic_if *interface;

	if (adapter->stopped)
		return 0;

	// Stop all interfaces
	for (idx=0; idx<hw->if_count; idx++) {
		interface = hw->interface[idx];
		PMD_INIT_LOG(INFO, "Stopping interface %d", idx);

		mqnic_all_port_deactivate(interface);
		mqnic_dev_deactive_queues(dev);
		mqnic_tx_cpl_queue_deactivate(interface);
		mqnic_rx_cpl_queue_deactivate(interface);
		mqnic_all_event_queue_deactivate(interface);
	}

	rte_delay_us_sleep(10000);
	mqnic_dev_clear_queues(dev);

	/* clear the recorded link status */
	memset(&link, 0, sizeof(link));
	rte_eth_linkstatus_set(dev, &link);

	adapter->stopped = true;
	dev->data->dev_started = 0;

	return 0;
}

int eth_mqnic_close(struct rte_eth_dev *dev)
{
	struct rte_eth_link link;
	int ret = 0;
	int idx;
	struct mqnic_adapter *adapter = MQNIC_DEV_PRIVATE(dev->data->dev_private);
	struct mqnic_hw *hw = &adapter->hw;
	struct mqnic_if *interface;

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	ret = eth_mqnic_stop(dev);

	// Close all interfaces
	for (idx=0; idx<hw->if_count; idx++) {
		interface = hw->interface[idx];
		PMD_INIT_LOG(INFO, "Closing interface %d", idx);

		mqnic_all_ports_destroy(interface);
		mqnic_dev_free_queues(dev);
		mqnic_tx_cpl_queue_destroy(interface);
		mqnic_rx_cpl_queue_destroy(interface);
		mqnic_all_event_queue_destroy(dev);
	}

	memset(&link, 0, sizeof(link));
	rte_eth_linkstatus_set(dev, &link);

	return ret;
}

/*
 * Reset PF device.
 */
static int
eth_mqnic_reset(struct rte_eth_dev *dev)
{
	int ret;

	/* When a DPDK PMD PF begin to reset PF port, it should notify all
	 * its VF to make them align with it. The detailed notification
	 * mechanism is PMD specific and is currently not implemented.
	 * To avoid unexpected behavior in VF, currently reset of PF with
	 * SR-IOV activation is not supported. It might be supported later.
	 */
	if (dev->data->sriov.active)
		return -ENOTSUP;

	ret = eth_mqnic_dev_uninit(dev);
	if (ret)
		return ret;

	ret = eth_mqnic_dev_init(dev);

	return ret;
}

static int
eth_mqnic_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *rte_stats)
{
	struct mqnic_priv *priv =
		MQNIC_DEV_PRIVATE_TO_PRIV(dev->data->dev_private);

	if (rte_stats == NULL)
		return -EINVAL;

	rte_stats->ipackets = priv->ipackets;
	rte_stats->opackets = priv->opackets;
	rte_stats->ibytes   = priv->ibytes;
	rte_stats->obytes   = priv->obytes;

	return 0;
}

static int
eth_mqnic_stats_reset(struct rte_eth_dev *dev)
{
	struct mqnic_priv *priv =
		MQNIC_DEV_PRIVATE_TO_PRIV(dev->data->dev_private);

	priv->ipackets = 0;
	priv->opackets = 0;
	priv->ibytes = 0;
	priv->obytes = 0;

	return 0;
}

static int
eth_mqnic_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{

	dev_info->min_rx_bufsize = 256; /* See BSIZE field of RCTL register. */
	dev_info->max_rx_pktlen  = 0x1000;//0x3FFF; /* See RLPML register. */
	dev_info->max_mac_addrs = 1;//hw->mac.rar_entry_count;
	dev_info->rx_queue_offload_capa = mqnic_get_rx_queue_offloads_capa(dev);
	dev_info->rx_offload_capa = mqnic_get_rx_port_offloads_capa(dev) |
				    dev_info->rx_queue_offload_capa;
	dev_info->tx_queue_offload_capa = mqnic_get_tx_queue_offloads_capa(dev);
	dev_info->tx_offload_capa = mqnic_get_tx_port_offloads_capa(dev) |
				    dev_info->tx_queue_offload_capa;

	dev_info->max_rx_queues = 16;
	dev_info->max_tx_queues = 16;

	dev_info->max_vmdq_pools = 0;

	dev_info->hash_key_size = IGB_HKEY_MAX_INDEX * sizeof(uint32_t);

	dev_info->default_rxconf = (struct rte_eth_rxconf) {
		.rx_thresh = {
			.pthresh = IGB_DEFAULT_RX_PTHRESH,
			.hthresh = IGB_DEFAULT_RX_HTHRESH,
			.wthresh = IGB_DEFAULT_RX_WTHRESH,
		},
		.rx_free_thresh = IGB_DEFAULT_RX_FREE_THRESH,
		.rx_drop_en = 0,
		.offloads = 0,
	};

	dev_info->default_txconf = (struct rte_eth_txconf) {
		.tx_thresh = {
			.pthresh = IGB_DEFAULT_TX_PTHRESH,
			.hthresh = IGB_DEFAULT_TX_HTHRESH,
			.wthresh = IGB_DEFAULT_TX_WTHRESH,
		},
		.offloads = 0,
	};

	dev_info->rx_desc_lim = rx_desc_lim;
	dev_info->tx_desc_lim = tx_desc_lim;

	dev_info->speed_capa = ETH_LINK_SPEED_100G;

	dev_info->max_mtu = dev_info->max_rx_pktlen - MQNIC_ETH_OVERHEAD;
	dev_info->min_mtu = RTE_ETHER_MIN_MTU;

	return 0;
}

static const uint32_t *
eth_mqnic_supported_ptypes_get(struct rte_eth_dev *dev)
{
	static const uint32_t ptypes[] = {
		/* refers to mqnic_rxd_pkt_info_to_pkt_type() */
		RTE_PTYPE_L2_ETHER,
		RTE_PTYPE_L3_IPV4,
		RTE_PTYPE_L3_IPV4_EXT,
		RTE_PTYPE_L3_IPV6,
		RTE_PTYPE_L3_IPV6_EXT,
		RTE_PTYPE_L4_TCP,
		RTE_PTYPE_L4_UDP,
		RTE_PTYPE_L4_SCTP,
		RTE_PTYPE_TUNNEL_IP,
		RTE_PTYPE_INNER_L3_IPV6,
		RTE_PTYPE_INNER_L3_IPV6_EXT,
		RTE_PTYPE_INNER_L4_TCP,
		RTE_PTYPE_INNER_L4_UDP,
		RTE_PTYPE_UNKNOWN
	};

	if (dev->rx_pkt_burst == eth_mqnic_recv_pkts ||
	    dev->rx_pkt_burst == eth_mqnic_recv_scattered_pkts)
		return ptypes;
	return NULL;
}

/* return 0 means link status changed, -1 means not changed */
static int
eth_mqnic_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct rte_eth_link link;

	RTE_SET_USED(wait_to_complete);

	memset(&link, 0, sizeof(link));

	/* Now we check if a transition has happened */
	link.link_duplex = ETH_LINK_FULL_DUPLEX;
	link.link_speed = ETH_SPEED_NUM_100G;
	link.link_status = ETH_LINK_UP;
	link.link_autoneg = 0;


	return rte_eth_linkstatus_set(dev, &link);
}

static int
eth_mqnic_promiscuous_enable(struct rte_eth_dev *dev)
{
	RTE_SET_USED(dev);
	return 0;
}

static int
eth_mqnic_promiscuous_disable(struct rte_eth_dev *dev)
{
	RTE_SET_USED(dev);
	return 0;
}

static int
eth_mqnic_mtu_set(struct rte_eth_dev *dev, uint16_t mtu)
{
	mqnic_set_interface_mtu(dev, mtu);
	return 0;
}

/* see mqnic_logs.c */
RTE_INIT(mqnic_init_log)
{
	mqnic_mqnic_init_log();
}

u32 mqnic_port_get_tx_status(struct mqnic_port *port)
{
	return MQNIC_DIRECT_READ_REG(port->port_ctrl_rb->regs, MQNIC_RB_PORT_CTRL_REG_TX_STATUS);
}

u32 mqnic_port_get_rx_status(struct mqnic_port *port)
{
	return MQNIC_DIRECT_READ_REG(port->port_ctrl_rb->regs, MQNIC_RB_PORT_CTRL_REG_RX_STATUS);
}

RTE_PMD_REGISTER_PCI(net_mqnic_igb, rte_mqnic_pmd);
RTE_PMD_REGISTER_PCI_TABLE(net_mqnic_igb, pci_id_mqnic_map);
RTE_PMD_REGISTER_KMOD_DEP(net_mqnic_igb, "* uio_pci_generic | vfio");
