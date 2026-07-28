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
 * The IN side forwards only while the host is actually draining the pipe,
 * giving FT232-style "port closed = data dropped" semantics without relying
 * on DTR (not every host app asserts it). Open detection: while idle a ZLP
 * sits armed on the IN endpoint; host serial drivers (cdc_acm/usbser) submit
 * IN URBs only while the port is open, so the ZLP completing proves an open
 * port (apps never see the empty read). Close detection: a transfer pending
 * longer than PUMP_HOST_IDLE_MS means the URB flow stopped; it is retracted
 * via usbd_ep_close/open (aborts the transfer, flushes the TX FIFO) and UART
 * RX is discarded until the next probe completes, so reopening the port
 * never replays stale bytes.
 *
 * USB -> UART (OUT): 8 slots of 64B per channel form a ring. The OUT-complete
 * ISR immediately re-arms the next free slot (no thread round-trip); the pump
 * thread coalesces filled slots into a 512B staging buffer, frees them right
 * away and hands the serial layer exactly one DMA node at a time (next batch
 * submitted from the TX-complete callback path). Keeping the serial v1 TX
 * data queue at depth <= 1 sidesteps its push-vs-DMADONE race (a lost
 * completion wedges the queue forever, observed as a hard TX stall at 2M+),
 * and lets baud reconfiguration quiesce TX cleanly. When all slots are busy
 * the OUT endpoint stays un-armed and the host sees NAK backpressure.
 *
 * Baud/format changes recorded by cdc_proto.c (USB ISR) are applied here in
 * thread context, since HAL_UART_Init must not run in an ISR; the pump defers
 * them until the in-flight TX node (if any) completes, so reconfiguration
 * never kills an active DMA (another lost-completion path).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtdevice.h>

#include "usbd_core.h"

#include "usb_bridge.h"

#define PUMP_IN_BUF_SIZE   512U /* one bulk transfer, 8 packets */
#define PUMP_OUT_SLOTS     8U   /* 64B OUT slots */
#define PUMP_TX_STAGE_SIZE (PUMP_OUT_SLOTS * USB_BRIDGE_BULK_MPS)
#define PUMP_HOST_IDLE_MS  500 /* pending IN with no completion -> port closed */

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
    SLOT_READY,      /* holds host data, waiting for TX staging */
};

struct bridge_pump {
    /* static wiring */
    const char *uart_name;
    uint8_t ch;
    uint8_t in_ep;
    uint8_t out_ep;
    uint32_t max_baud;  /* hardware clamp (BRR divisor >= 1 in OVER8) */
    uint16_t rx_bufsz;  /* serial ring size, fixed at open time */
    const struct usb_endpoint_descriptor *in_desc; /* for retract reopen */

    rt_device_t uart;
    struct rt_event ev;

    /* USB -> UART ring; slots advance FREE -> ARMED -> READY -> FREE
     * strictly in ring order (freed when copied into the TX stage).
     * dwc2 requires 4-byte aligned transfer buffers. */
    uint8_t out_buf[PUMP_OUT_SLOTS][USB_BRIDGE_BULK_MPS] __attribute__((aligned(4)));
    volatile uint8_t out_state[PUMP_OUT_SLOTS];
    volatile uint8_t out_len[PUMP_OUT_SLOTS];
    volatile uint8_t out_arm_idx;   /* next slot to arm (ISR owned) */
    uint8_t out_drain_idx;          /* next slot to stage (thread owned) */
    volatile uint8_t out_stalled;   /* no free slot at last completion: host NAKed */

    /* single in-flight TX node: READY slots coalesce here, the serial v1 TX
     * data queue never holds more than this one buffer (see header comment) */
    uint8_t tx_stage[PUMP_TX_STAGE_SIZE] __attribute__((aligned(4)));
    volatile uint8_t tx_inflight;

    /* UART -> USB assembly */
    uint8_t in_buf[PUMP_IN_BUF_SIZE] __attribute__((aligned(4)));
    volatile uint8_t in_busy;

    /* host-read presence detection (see header comment) */
    volatile uint8_t host_reading; /* host is draining the IN pipe (port open) */
    volatile uint8_t probe_armed;  /* read-probe ZLP pending on the IN ep */
    rt_tick_t in_start;            /* tick when the in-flight IN was armed */

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
    uint32_t tx_nodes;       /* TX DMA nodes submitted */
    uint32_t tx_dones;       /* TX DMA completions observed */
    uint32_t host_opens;     /* port-open detections (probe/data completed) */
    uint32_t host_retracts;  /* port-close detections (IN retracted) */
};

/* Minimal bulk IN descriptors so a retract can usbd_ep_open again; must
 * match the config descriptor in bridge_desc.c. */
static const struct usb_endpoint_descriptor pump_a_in_desc = {
    .bLength = USB_SIZEOF_ENDPOINT_DESC,
    .bDescriptorType = USB_DESCRIPTOR_TYPE_ENDPOINT,
    .bEndpointAddress = USB_BRIDGE_A_IN_EP,
    .bmAttributes = USB_ENDPOINT_TYPE_BULK,
    .wMaxPacketSize = USB_BRIDGE_BULK_MPS,
    .bInterval = 0,
};
static const struct usb_endpoint_descriptor pump_b_in_desc = {
    .bLength = USB_SIZEOF_ENDPOINT_DESC,
    .bDescriptorType = USB_DESCRIPTOR_TYPE_ENDPOINT,
    .bEndpointAddress = USB_BRIDGE_B_IN_EP,
    .bmAttributes = USB_ENDPOINT_TYPE_BULK,
    .wMaxPacketSize = USB_BRIDGE_BULK_MPS,
    .bInterval = 0,
};

static struct bridge_pump pump_a = {
    .uart_name = "uart6",
    .ch = USB_BRIDGE_CH_A,
    .in_ep = USB_BRIDGE_A_IN_EP,
    .out_ep = USB_BRIDGE_A_OUT_EP,
    .max_baud = 11250000UL, /* USART6 on APB2 90MHz, OVER8 floor */
    .rx_bufsz = 8192,
    .in_desc = &pump_a_in_desc,
};

static struct bridge_pump pump_b = {
    .uart_name = "uart1",
    .ch = USB_BRIDGE_CH_B,
    .in_ep = USB_BRIDGE_B_IN_EP,
    .out_ep = USB_BRIDGE_B_OUT_EP,
    .max_baud = 11250000UL, /* USART1 on APB2 90MHz, OVER8 floor */
    .rx_bufsz = 8192,
    .in_desc = &pump_b_in_desc,
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

    /* any completion (data, terminator ZLP or read probe) proves the host
     * is draining the pipe: IN URBs only exist while the port is open */
    p->probe_armed = 0;
    p->in_busy = 0;
    if (!p->host_reading) {
        p->host_reading = 1;
        p->host_opens++;
    }
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
    pump_a.tx_dones++;
    pump_a.tx_inflight = 0;
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
    pump_b.tx_dones++;
    pump_b.tx_inflight = 0;
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
    if (p->tx_inflight) {
        return; /* defer: reconfig would kill the active TX DMA and its
                 * completion; retried on the next TX-done event */
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

/* Coalesce READY slots into the staging buffer and submit one TX DMA node.
 * Slots free as soon as they are copied, so the OUT endpoint resumes before
 * the UART is done draining. */
static void pump_drain_out_slots(struct bridge_pump *p)
{
    uint32_t staged = 0;

    if (p->tx_inflight) {
        return; /* one node in flight max, next batch on TX-done */
    }

    while (p->out_state[p->out_drain_idx] == SLOT_READY &&
           staged + USB_BRIDGE_BULK_MPS <= PUMP_TX_STAGE_SIZE) {
        uint8_t idx = p->out_drain_idx;
        rt_size_t len = p->out_len[idx];

        rt_memcpy(&p->tx_stage[staged], (const void *)p->out_buf[idx], len);
        staged += len;
        p->out_state[idx] = SLOT_FREE;
        p->out_drain_idx = (uint8_t)((idx + 1U) % PUMP_OUT_SLOTS);
    }

    if (p->out_stalled) {
        pump_try_arm_out(p);
    }
    if (staged == 0) {
        return;
    }

    p->tx_inflight = 1;
    p->tx_nodes++;
    if (rt_device_write(p->uart, 0, p->tx_stage, staged) != (rt_ssize_t)staged) {
        p->tx_inflight = 0; /* uart write failed: drop the batch */
    } else {
        p->usb_to_uart_bytes += staged;
    }
}

static void pump_send_uart_data(struct bridge_pump *p)
{
    if (p->in_busy || !p->configured || !p->host_reading) {
        return;
    }

    rt_size_t got = rt_device_read(p->uart, 0, p->in_buf, PUMP_IN_BUF_SIZE);

    if (got == 0) {
        return;
    }

    p->uart_to_usb_bytes += got;
    p->in_xfers++;
    p->in_last_total = got;
    p->in_start = rt_tick_get();
    p->in_busy = 1;
    bridge_ep_start_write(p->in_ep, p->in_buf, got);
}

/* Nobody is listening: keep the serial ring empty so a later open never
 * replays history (what a hardware bridge does when its tiny buffer
 * overflows and the driver purges on open). in_buf doubles as the discard
 * scratch, safe because no IN transfer is in flight while idle. */
static void pump_discard_uart_rx(struct bridge_pump *p)
{
    if (p->in_busy) {
        return;
    }
    while (rt_device_read(p->uart, 0, p->in_buf, PUMP_IN_BUF_SIZE) > 0) {
    }
}

/* Idle side of open detection: drop UART RX and keep one ZLP probe armed on
 * the IN endpoint; its completion (pump_usb_in_complete) flips host_reading.
 * The recheck and the arm share one interrupt-off section: if the previous
 * probe completes between them the ISR flips host_reading and arming again
 * would double-arm the endpoint under an imminent data write. */
static void pump_probe_host(struct bridge_pump *p)
{
    rt_base_t level;

    if (p->host_reading) {
        return;
    }
    pump_discard_uart_rx(p);

    level = rt_hw_interrupt_disable();
    if (p->configured && !p->host_reading && !p->probe_armed && !p->in_busy) {
        p->probe_armed = 1;
        usbd_ep_start_write(USB_BRIDGE_BUSID, p->in_ep, RT_NULL, 0);
    }
    rt_hw_interrupt_enable(level);
}

/* Close detection: an IN transfer nobody reads within PUMP_HOST_IDLE_MS
 * means the URB flow stopped (port closed, or host throttled long enough
 * that a hardware bridge would be dropping too). Retract it so the stale
 * payload never reaches the next open: ep close aborts the transfer, ep
 * open re-registers it and flushes the TX FIFO. Interrupt-off for the same
 * dwc2 shared-register rule as bridge_ep_start_*; the in_busy recheck
 * closes the race with a completion firing just before the close. */
static void pump_check_host_gone(struct bridge_pump *p)
{
    rt_base_t level;

    if (!p->host_reading || !p->in_busy || !p->configured) {
        return;
    }
    if (rt_tick_get() - p->in_start < rt_tick_from_millisecond(PUMP_HOST_IDLE_MS)) {
        return;
    }

    level = rt_hw_interrupt_disable();
    if (p->in_busy) {
        usbd_ep_close(USB_BRIDGE_BUSID, p->in_ep);
        usbd_ep_open(USB_BRIDGE_BUSID, p->in_desc);
        p->in_busy = 0;
        p->host_reading = 0;
        p->host_retracts++;
    }
    rt_hw_interrupt_enable(level);
}

static void pump_restart(struct bridge_pump *p)
{
    for (uint8_t i = 0; i < PUMP_OUT_SLOTS; i++) {
        p->out_state[i] = SLOT_FREE;
        p->out_len[i] = 0;
    }
    p->out_arm_idx = 0;
    p->out_drain_idx = 0;
    p->out_stalled = 0;
    /* tx_inflight is left alone: an in-flight DMA node completes on its own
     * and clears it via the TX-done callback */
    p->in_busy = 0;
    /* bus reset killed any armed transfer with it; pump_probe_host re-arms
     * a fresh read probe once configured */
    p->host_reading = 0;
    p->probe_armed = 0;
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

        pump_check_host_gone(p);
        pump_probe_host(p);
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

        rt_kprintf("ch%c pump: u2h=%u h2u=%u nak=%u clamp=%u cfg=%u rd=%u opens=%u retracts=%u\n",
                   'A' + p->ch, p->uart_to_usb_bytes, p->usb_to_uart_bytes,
                   p->nak_backpressure, p->clamped_baud, p->configured,
                   p->host_reading, p->host_opens, p->host_retracts);
        rt_kprintf("      in: xfers=%u done=%u zlp=%u busy=%u last=%u | out: done=%u stall=%u\n",
                   p->in_xfers, p->in_completes, p->in_zlps, p->in_busy,
                   p->in_last_total, p->out_completes, p->out_stalled);
        rt_kprintf("      tx: nodes=%u done=%u inflight=%u | slots:",
                   p->tx_nodes, p->tx_dones, p->tx_inflight);
        for (uint8_t s = 0; s < PUMP_OUT_SLOTS; s++) {
            rt_kprintf(" %u", p->out_state[s]);
        }
        rt_kprintf(" arm=%u drain=%u\n", p->out_arm_idx, p->out_drain_idx);
    }
}
