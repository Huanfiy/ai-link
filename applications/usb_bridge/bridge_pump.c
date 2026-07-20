/*
 * bridge_pump.c - Bidirectional data pumps between CDC bulk pairs and UARTs.
 *
 * One thread per CDC channel moves data both ways:
 *
 * UART -> USB (IN): payload read from the serial DMA ring goes out verbatim
 * (CDC is a transparent pipe, no per-packet framing), up to 512B per bulk
 * transfer. When a transfer ends exactly on a 64B packet boundary a ZLP
 * follows so hosts reading with multi-packet URBs (Windows usbser) see the
 * transfer complete instead of waiting for more data.
 *
 * USB -> UART (OUT): 8 slots of 64B per channel form a ring. The OUT-complete
 * ISR immediately re-arms the next free slot (no thread round-trip); the pump
 * thread queues filled slots into the UART TX DMA and slots are freed by the
 * TX-complete callback. When all slots are busy the OUT endpoint stays
 * un-armed and the host sees NAK backpressure.
 *
 * Baud/format changes recorded by cdc_proto.c (USB ISR) are applied here in
 * thread context, since HAL_UART_Init must not run in an ISR.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtdevice.h>

#include "usbd_core.h"

#include "usb_bridge.h"

#define PUMP_IN_BUF_SIZE   512U /* one bulk transfer, 8 packets */
#define PUMP_OUT_SLOTS     8U   /* 64B OUT slots, matches serial DMA queue depth */

#define PUMP_EV_USB_OUT   (1U << 0) /* one or more OUT slots filled */
#define PUMP_EV_USB_IN    (1U << 1) /* IN transfer completed */
#define PUMP_EV_UART_RX   (1U << 2) /* serial RX data available */
#define PUMP_EV_UART_TX   (1U << 3) /* one UART DMA node consumed */
#define PUMP_EV_RESTART   (1U << 4) /* USB configured/reset: resync state */
#define PUMP_EV_ALL       (PUMP_EV_USB_OUT | PUMP_EV_USB_IN | PUMP_EV_UART_RX | \
                           PUMP_EV_UART_TX | PUMP_EV_RESTART)

enum pump_slot_state {
    SLOT_FREE = 0,   /* available for usbd_ep_start_read */
    SLOT_ARMED,      /* owned by the USB core, read in progress */
    SLOT_READY,      /* holds host data, waiting for UART queueing */
    SLOT_QUEUED,     /* sitting in the UART TX DMA queue */
};

struct bridge_pump {
    /* static wiring */
    const char *uart_name;
    uint8_t ch;
    uint8_t in_ep;
    uint8_t out_ep;
    uint32_t max_baud;  /* hardware clamp (BRR divisor >= 1 in OVER8) */
    uint16_t rx_bufsz;  /* serial ring size, fixed at open time */

    rt_device_t uart;
    struct rt_event ev;

    /* USB -> UART ring; slots advance FREE -> ARMED -> READY -> QUEUED -> FREE
     * strictly in ring order, tracked by three cursors.
     * dwc2 requires 4-byte aligned transfer buffers. */
    uint8_t out_buf[PUMP_OUT_SLOTS][USB_BRIDGE_BULK_MPS] __attribute__((aligned(4)));
    volatile uint8_t out_state[PUMP_OUT_SLOTS];
    volatile uint8_t out_len[PUMP_OUT_SLOTS];
    volatile uint8_t out_arm_idx;   /* next slot to arm (ISR owned) */
    uint8_t out_drain_idx;          /* next slot to hand to UART (thread owned) */
    uint8_t out_free_idx;           /* oldest QUEUED slot, freed on TX-done */
    volatile uint8_t out_stalled;   /* no free slot at last completion: host NAKed */
    volatile uint32_t tx_done_count; /* UART DMA nodes consumed (callback) */
    uint32_t tx_reaped;              /* thread-side match for tx_done_count */

    /* UART -> USB assembly */
    uint8_t in_buf[PUMP_IN_BUF_SIZE] __attribute__((aligned(4)));
    volatile uint8_t in_busy;

    volatile uint8_t configured;
    uint32_t applied_seq;

    /* stats for usbbr_stat */
    uint32_t usb_to_uart_bytes;
    uint32_t uart_to_usb_bytes;
    uint32_t nak_backpressure;
    uint32_t clamped_baud;
    uint32_t in_xfers;       /* IN transfers started */
    uint32_t in_completes;   /* IN complete callbacks */
    uint32_t in_zlps;        /* trailing ZLPs sent */
    uint32_t out_completes;  /* OUT complete callbacks */
    uint32_t in_last_total;  /* size of last IN transfer */
};

static struct bridge_pump pump_a = {
    .uart_name = "uart6",
    .ch = USB_BRIDGE_CH_A,
    .in_ep = USB_BRIDGE_A_IN_EP,
    .out_ep = USB_BRIDGE_A_OUT_EP,
    .max_baud = 11250000UL, /* USART6 on APB2 90MHz, OVER8 floor */
    .rx_bufsz = 8192,
};

static struct bridge_pump pump_b = {
    .uart_name = "uart1",
    .ch = USB_BRIDGE_CH_B,
    .in_ep = USB_BRIDGE_B_IN_EP,
    .out_ep = USB_BRIDGE_B_OUT_EP,
    .max_baud = 11250000UL, /* USART1 on APB2 90MHz, OVER8 floor */
    .rx_bufsz = 8192,
};

static struct bridge_pump *const pumps[] = { &pump_a, &pump_b };
#define PUMP_COUNT (sizeof(pumps) / sizeof(pumps[0]))

static rt_uint8_t pump_a_stack[2048];
static struct rt_thread pump_a_thread;
static rt_uint8_t pump_b_stack[2048];
static struct rt_thread pump_b_thread;

static struct bridge_pump *pump_from_ep(uint8_t ep)
{
    for (uint8_t i = 0; i < PUMP_COUNT; i++) {
        if (pumps[i]->in_ep == ep || pumps[i]->out_ep == ep) {
            return pumps[i];
        }
    }
    return RT_NULL;
}

/* ---------------- USB ISR side ---------------- */

/* The dwc2 driver read-modify-writes shared registers (DIEPEMPMSK) in both
 * usbd_ep_start_* and its ISR. Calls from thread context must therefore be
 * serialized against the USB ISR or the unmask bit can be lost, wedging the
 * transfer forever. Interrupt-off sections are nesting-safe, so wrapping
 * ISR-side calls too costs nothing. */
static void bridge_ep_start_write(uint8_t ep, const uint8_t *data, uint32_t len)
{
    rt_base_t level = rt_hw_interrupt_disable();

    usbd_ep_start_write(USB_BRIDGE_BUSID, ep, data, len);
    rt_hw_interrupt_enable(level);
}

static void bridge_ep_start_read(uint8_t ep, uint8_t *data, uint32_t len)
{
    rt_base_t level = rt_hw_interrupt_disable();

    usbd_ep_start_read(USB_BRIDGE_BUSID, ep, data, len);
    rt_hw_interrupt_enable(level);
}

/* Arm the next free OUT slot; called from the OUT-complete ISR and, on
 * stall recovery, from the pump thread (endpoint is idle then, no race). */
static void pump_try_arm_out(struct bridge_pump *p)
{
    uint8_t idx = p->out_arm_idx;

    if (!p->configured) {
        return;
    }
    if (p->out_state[idx] != SLOT_FREE) {
        p->out_stalled = 1;
        p->nak_backpressure++;
        return;
    }
    p->out_state[idx] = SLOT_ARMED;
    p->out_stalled = 0;
    bridge_ep_start_read(p->out_ep, p->out_buf[idx], USB_BRIDGE_BULK_MPS);
}

static void pump_usb_out_complete(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    struct bridge_pump *p = pump_from_ep(ep);

    (void)busid;
    if (p == RT_NULL) {
        return;
    }

    uint8_t idx = p->out_arm_idx;

    p->out_completes++;
    if (nbytes == 0) { /* ZLP carries nothing: rearm the same slot */
        p->out_state[idx] = SLOT_FREE;
        pump_try_arm_out(p);
        return;
    }

    p->out_len[idx] = (uint8_t)nbytes;
    p->out_state[idx] = SLOT_READY;
    p->out_arm_idx = (uint8_t)((idx + 1U) % PUMP_OUT_SLOTS);
    pump_try_arm_out(p);
    rt_event_send(&p->ev, PUMP_EV_USB_OUT);
}

static void pump_usb_in_complete(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    struct bridge_pump *p = pump_from_ep(ep);

    (void)busid;
    if (p == RT_NULL) {
        return;
    }
    p->in_completes++;

    /* transfer ended on a full packet: chase it with a ZLP so the host URB
     * completes now (nbytes == 0 is that ZLP finishing) */
    if (nbytes != 0 && (nbytes % USB_BRIDGE_BULK_MPS) == 0 && p->configured) {
        p->in_zlps++;
        usbd_ep_start_write(USB_BRIDGE_BUSID, p->in_ep, RT_NULL, 0);
        return;
    }
    p->in_busy = 0;
    rt_event_send(&p->ev, PUMP_EV_USB_IN);
}

/* ---------------- UART callbacks (ISR context) ---------------- */

static rt_err_t pump_uart_rx_ind_a(rt_device_t dev, rt_size_t size)
{
    (void)dev;
    (void)size;
    rt_event_send(&pump_a.ev, PUMP_EV_UART_RX);
    return RT_EOK;
}

static rt_err_t pump_uart_tx_done_a(rt_device_t dev, void *buffer)
{
    (void)dev;
    (void)buffer;
    pump_a.tx_done_count++;
    rt_event_send(&pump_a.ev, PUMP_EV_UART_TX);
    return RT_EOK;
}

static rt_err_t pump_uart_rx_ind_b(rt_device_t dev, rt_size_t size)
{
    (void)dev;
    (void)size;
    rt_event_send(&pump_b.ev, PUMP_EV_UART_RX);
    return RT_EOK;
}

static rt_err_t pump_uart_tx_done_b(rt_device_t dev, void *buffer)
{
    (void)dev;
    (void)buffer;
    pump_b.tx_done_count++;
    rt_event_send(&pump_b.ev, PUMP_EV_UART_TX);
    return RT_EOK;
}

/* ---------------- pump thread ---------------- */

static void pump_apply_line_cfg(struct bridge_pump *p)
{
    struct cdc_channel_state *st = &g_cdc_ch[p->ch];
    uint32_t seq = st->line_seq;
    struct serial_configure cfg = RT_SERIAL_CONFIG_DEFAULT;
    uint32_t baud = st->line.baud;

    if (seq == p->applied_seq || baud == 0) {
        return;
    }

    if (baud > p->max_baud) {
        rt_kprintf("[usb_bridge] ch%c: baud %u beyond hw, clamped to %u\n",
                   'A' + p->ch, baud, p->max_baud);
        baud = p->max_baud;
        p->clamped_baud++;
    }

    cfg.baud_rate = baud;
    cfg.bufsz = p->rx_bufsz; /* must equal the opened size or CONFIG is EBUSY */

    /* STM32 word length is 8/9 bits: 7 data bits work only with parity */
    cfg.data_bits = (st->line.data_bits == 7) ? DATA_BITS_7 : DATA_BITS_8;
    switch (st->line.parity) {
    case 1:
        cfg.parity = PARITY_ODD;
        break;
    case 2:
        cfg.parity = PARITY_EVEN;
        break;
    default: /* none; mark/space unsupported on STM32 */
        cfg.parity = PARITY_NONE;
        break;
    }
    /* CDC bCharFormat: 0 = 1 stop, 1 = 1.5 (unsupported, use 2), 2 = 2 */
    cfg.stop_bits = (st->line.stop_bits != 0) ? STOP_BITS_2 : STOP_BITS_1;

    if (rt_device_control(p->uart, RT_DEVICE_CTRL_CONFIG, &cfg) != RT_EOK) {
        rt_kprintf("[usb_bridge] ch%c: uart reconfig failed (baud %u)\n",
                   'A' + p->ch, baud);
        return;
    }
    p->applied_seq = seq;
}

static void pump_drain_out_slots(struct bridge_pump *p)
{
    while (p->out_state[p->out_drain_idx] == SLOT_READY) {
        uint8_t idx = p->out_drain_idx;
        rt_size_t len = p->out_len[idx];

        p->out_state[idx] = SLOT_QUEUED;
        /* async: pointer enters the serial TX DMA queue (depth 8 >= slots),
         * slot freed in pump_reap_tx() after the TX-done callback */
        if (rt_device_write(p->uart, 0, p->out_buf[idx], len) != (rt_ssize_t)len) {
            p->out_state[idx] = SLOT_FREE; /* uart write failed: drop */
        } else {
            p->usb_to_uart_bytes += len;
        }
        p->out_drain_idx = (uint8_t)((idx + 1U) % PUMP_OUT_SLOTS);
    }
}

/* Free QUEUED slots as UART DMA completions arrive; both sides advance in
 * strict ring order, so out_free_idx always points at the oldest one. */
static void pump_reap_tx(struct bridge_pump *p)
{
    while (p->tx_reaped != p->tx_done_count) {
        if (p->out_state[p->out_free_idx] != SLOT_QUEUED) {
            /* completion for a buffer not from this ring (cannot happen in
             * steady state; guard against restart races) */
            p->tx_reaped = p->tx_done_count;
            break;
        }
        p->out_state[p->out_free_idx] = SLOT_FREE;
        p->out_free_idx = (uint8_t)((p->out_free_idx + 1U) % PUMP_OUT_SLOTS);
        p->tx_reaped++;
    }
    if (p->out_stalled) {
        pump_try_arm_out(p);
    }
}

static void pump_send_uart_data(struct bridge_pump *p)
{
    if (p->in_busy || !p->configured) {
        return;
    }

    rt_size_t got = rt_device_read(p->uart, 0, p->in_buf, PUMP_IN_BUF_SIZE);

    if (got == 0) {
        return;
    }

    p->uart_to_usb_bytes += got;
    p->in_xfers++;
    p->in_last_total = got;
    p->in_busy = 1;
    bridge_ep_start_write(p->in_ep, p->in_buf, got);
}

static void pump_restart(struct bridge_pump *p)
{
    for (uint8_t i = 0; i < PUMP_OUT_SLOTS; i++) {
        p->out_state[i] = SLOT_FREE;
        p->out_len[i] = 0;
    }
    p->out_arm_idx = 0;
    p->out_drain_idx = 0;
    p->out_free_idx = 0;
    p->out_stalled = 0;
    p->tx_reaped = p->tx_done_count;
    p->in_busy = 0;
    if (p->configured) {
        pump_try_arm_out(p);
    }
}

static void pump_entry(void *param)
{
    struct bridge_pump *p = (struct bridge_pump *)param;
    rt_uint32_t ev;

    while (1) {
        /* modest timeout as a safety net: all fast paths are event-driven */
        if (rt_event_recv(&p->ev, PUMP_EV_ALL,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          rt_tick_from_millisecond(10), &ev) != RT_EOK) {
            ev = 0;
        }

        if (ev & PUMP_EV_RESTART) {
            pump_restart(p);
        }

        pump_apply_line_cfg(p);

        pump_reap_tx(p);
        pump_drain_out_slots(p);
        pump_send_uart_data(p);
    }
}

/* ---------------- wiring ---------------- */

static struct usbd_endpoint pump_a_in_ep = {
    .ep_addr = USB_BRIDGE_A_IN_EP,
    .ep_cb = pump_usb_in_complete,
};
static struct usbd_endpoint pump_a_out_ep = {
    .ep_addr = USB_BRIDGE_A_OUT_EP,
    .ep_cb = pump_usb_out_complete,
};
static struct usbd_endpoint pump_b_in_ep = {
    .ep_addr = USB_BRIDGE_B_IN_EP,
    .ep_cb = pump_usb_in_complete,
};
static struct usbd_endpoint pump_b_out_ep = {
    .ep_addr = USB_BRIDGE_B_OUT_EP,
    .ep_cb = pump_usb_out_complete,
};

/* CDC notify interrupt INs: mandatory in the descriptor, never used (no
 * SerialState reports); a completion callback is still required. */
static void pump_notify_in_complete(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
}

static struct usbd_endpoint pump_a_notify_ep = {
    .ep_addr = USB_BRIDGE_A_NOTIFY_EP,
    .ep_cb = pump_notify_in_complete,
};
static struct usbd_endpoint pump_b_notify_ep = {
    .ep_addr = USB_BRIDGE_B_NOTIFY_EP,
    .ep_cb = pump_notify_in_complete,
};

void bridge_pump_register_endpoints(void)
{
    usbd_add_endpoint(USB_BRIDGE_BUSID, &pump_a_in_ep);
    usbd_add_endpoint(USB_BRIDGE_BUSID, &pump_a_out_ep);
    usbd_add_endpoint(USB_BRIDGE_BUSID, &pump_b_in_ep);
    usbd_add_endpoint(USB_BRIDGE_BUSID, &pump_b_out_ep);
    usbd_add_endpoint(USB_BRIDGE_BUSID, &pump_a_notify_ep);
    usbd_add_endpoint(USB_BRIDGE_BUSID, &pump_b_notify_ep);
}

void bridge_pump_notify_event(uint8_t event)
{
    for (uint8_t i = 0; i < PUMP_COUNT; i++) {
        struct bridge_pump *p = pumps[i];

        switch (event) {
        case USBD_EVENT_CONFIGURED:
            p->configured = 1;
            rt_event_send(&p->ev, PUMP_EV_RESTART);
            break;
        case USBD_EVENT_RESET:
        case USBD_EVENT_DISCONNECTED:
            p->configured = 0;
            rt_event_send(&p->ev, PUMP_EV_RESTART);
            break;
        default:
            break;
        }
    }
}

static rt_err_t pump_open_uart(struct bridge_pump *p,
                               rt_err_t (*rx_ind)(rt_device_t, rt_size_t),
                               rt_err_t (*tx_done)(rt_device_t, void *))
{
    struct serial_configure cfg = RT_SERIAL_CONFIG_DEFAULT;
    rt_err_t err;

    p->uart = rt_device_find(p->uart_name);
    if (p->uart == RT_NULL) {
        rt_kprintf("[usb_bridge] %s not found\n", p->uart_name);
        return -RT_ERROR;
    }

    /* ring size must be set before open; it cannot change afterwards */
    cfg.bufsz = p->rx_bufsz;
    err = rt_device_control(p->uart, RT_DEVICE_CTRL_CONFIG, &cfg);
    if (err != RT_EOK) {
        return err;
    }

    rt_device_set_rx_indicate(p->uart, rx_ind);
    rt_device_set_tx_complete(p->uart, tx_done);

    err = rt_device_open(p->uart, RT_DEVICE_FLAG_RDWR |
                                  RT_DEVICE_FLAG_DMA_RX | RT_DEVICE_FLAG_DMA_TX);
    if (err != RT_EOK) {
        rt_kprintf("[usb_bridge] open %s failed: %d\n", p->uart_name, err);
    }
    return err;
}

rt_err_t bridge_pump_start(void)
{
    rt_err_t err;

    rt_event_init(&pump_a.ev, "usbbrA", RT_IPC_FLAG_FIFO);
    rt_event_init(&pump_b.ev, "usbbrB", RT_IPC_FLAG_FIFO);

    err = pump_open_uart(&pump_a, pump_uart_rx_ind_a, pump_uart_tx_done_a);
    if (err != RT_EOK) {
        return err;
    }
    err = pump_open_uart(&pump_b, pump_uart_rx_ind_b, pump_uart_tx_done_b);
    if (err != RT_EOK) {
        return err;
    }

    rt_thread_init(&pump_a_thread, "usbbrA", pump_entry, &pump_a,
                   pump_a_stack, sizeof(pump_a_stack), 8, 10);
    rt_thread_startup(&pump_a_thread);
    rt_thread_init(&pump_b_thread, "usbbrB", pump_entry, &pump_b,
                   pump_b_stack, sizeof(pump_b_stack), 8, 10);
    rt_thread_startup(&pump_b_thread);
    return RT_EOK;
}

void bridge_pump_dump_ep(void)
{
#define OTG_R(off) (*(volatile uint32_t *)(USB_BRIDGE_OTG_FS_BASE + (off)))
    for (uint8_t i = 1; i <= 5; i++) {
        rt_kprintf("IN%u: DIEPCTL=%08x DIEPINT=%08x DIEPTSIZ=%08x DTXFSTS=%08x\n",
                   i, OTG_R(0x900 + 0x20 * i), OTG_R(0x908 + 0x20 * i),
                   OTG_R(0x910 + 0x20 * i), OTG_R(0x918 + 0x20 * i));
    }
    for (uint8_t i = 1; i <= 3; i++) {
        rt_kprintf("OUT%u: DOEPCTL=%08x DOEPINT=%08x DOEPTSIZ=%08x\n",
                   i, OTG_R(0xB00 + 0x20 * i), OTG_R(0xB08 + 0x20 * i),
                   OTG_R(0xB10 + 0x20 * i));
    }
    rt_kprintf("DIEPEMPMSK=%08x DAINT=%08x DAINTMSK=%08x GINTSTS=%08x GINTMSK=%08x\n",
               OTG_R(0x834), OTG_R(0x818), OTG_R(0x81C), OTG_R(0x014), OTG_R(0x018));
    /* GRXSTSR (0x01C) peeks the RX FIFO status without popping it */
    rt_kprintf("GRXSTSR=%08x DSTS=%08x DCTL=%08x\n",
               OTG_R(0x01C), OTG_R(0x808), OTG_R(0x804));
}

void bridge_pump_stat(void)
{
    for (uint8_t i = 0; i < PUMP_COUNT; i++) {
        struct bridge_pump *p = pumps[i];

        rt_kprintf("ch%c pump: u2h=%u h2u=%u nak=%u clamp=%u cfg=%u\n",
                   'A' + p->ch, p->uart_to_usb_bytes, p->usb_to_uart_bytes,
                   p->nak_backpressure, p->clamped_baud, p->configured);
        rt_kprintf("      in: xfers=%u done=%u zlp=%u busy=%u last=%u | out: done=%u stall=%u\n",
                   p->in_xfers, p->in_completes, p->in_zlps, p->in_busy,
                   p->in_last_total, p->out_completes, p->out_stalled);
        rt_kprintf("      tx: dma_done=%u reaped=%u | slots:", p->tx_done_count, p->tx_reaped);
        for (uint8_t s = 0; s < PUMP_OUT_SLOTS; s++) {
            rt_kprintf(" %u", p->out_state[s]);
        }
        rt_kprintf(" arm=%u drain=%u free=%u\n",
                   p->out_arm_idx, p->out_drain_idx, p->out_free_idx);
    }
}
