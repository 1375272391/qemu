/*
 * A bus for connecting virtio serial and console ports
 *
 * Copyright (C) 2009 Red Hat, Inc.
 *
 * Author(s):
 *  Amit Shah <amit.shah@redhat.com>
 *
 * Some earlier parts are:
 *  Copyright IBM, Corp. 2008
 * authored by
 *  Christian Ehrhardt <ehrhardt@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "monitor.h"
#include "qemu-queue.h"
#include "sysbus.h"
#include "virtio-serial.h"

/* The virtio-serial bus on top of which the ports will ride as devices */
struct VirtIOSerialBus {
    BusState qbus;

    /* This is the parent device that provides the bus for ports. */
    VirtIOSerial *vser;

    /* The maximum number of ports that can ride on top of this bus */
    uint32_t max_nr_ports;
};

struct VirtIOSerial {
    VirtIODevice vdev;

    VirtQueue *c_ivq, *c_ovq;
    /* Arrays of ivqs and ovqs: one per port */
    VirtQueue **ivqs, **ovqs;

    VirtIOSerialBus *bus;

    QTAILQ_HEAD(, VirtIOSerialPort) ports;

    /* bitmap for identifying active ports */
    uint32_t *ports_map;

    struct virtio_console_config config;
};

static VirtIOSerialPort *find_port_by_id(VirtIOSerial *vser, uint32_t id)
{
    VirtIOSerialPort *port;

    if (id == VIRTIO_CONSOLE_BAD_ID) {
        return NULL;
    }

    QTAILQ_FOREACH(port, &vser->ports, next) {
        if (port->id == id)
            return port;
    }
    return NULL;
}

static VirtIOSerialPort *find_port_by_vq(VirtIOSerial *vser, VirtQueue *vq)
{
    VirtIOSerialPort *port;

    QTAILQ_FOREACH(port, &vser->ports, next) {
        if (port->ivq == vq || port->ovq == vq)
            return port;
    }
    return NULL;
}

static bool use_multiport(VirtIOSerial *vser)
{
    return vser->vdev.guest_features & (1 << VIRTIO_CONSOLE_F_MULTIPORT);
}

static size_t write_to_port(VirtIOSerialPort *port,
                            const uint8_t *buf, size_t size)
{
    VirtQueueElement elem;
    VirtQueue *vq;
    size_t offset = 0;
    size_t len = 0;

    vq = port->ivq;
    if (!virtio_queue_ready(vq)) {
        return 0;
    }

    while (offset < size) {
        int i;

        if (!virtqueue_pop(vq, &elem)) {
            break;
        }

        for (i = 0; offset < size && i < elem.in_num; i++) {
            len = MIN(elem.in_sg[i].iov_len, size - offset);

            memcpy(elem.in_sg[i].iov_base, buf + offset, len);
            offset += len;
        }
        virtqueue_push(vq, &elem, len);
    }

    virtio_notify(&port->vser->vdev, vq);
    return offset;
}

static size_t send_control_msg(VirtIOSerialPort *port, void *buf, size_t len)
{
    VirtQueueElement elem;
    VirtQueue *vq;
    struct virtio_console_control *cpkt;

    vq = port->vser->c_ivq;
    if (!virtio_queue_ready(vq)) {
        return 0;
    }
    if (!virtqueue_pop(vq, &elem)) {
        return 0;
    }

    cpkt = (struct virtio_console_control *)buf;
    stl_p(&cpkt->id, port->id);
    memcpy(elem.in_sg[0].iov_base, buf, len);

    virtqueue_push(vq, &elem, len);
    virtio_notify(&port->vser->vdev, vq);
    return len;
}

static size_t send_control_event(VirtIOSerialPort *port, uint16_t event,
                                 uint16_t value)
{
    struct virtio_console_control cpkt;

    stw_p(&cpkt.event, event);
    stw_p(&cpkt.value, value);

    return send_control_msg(port, &cpkt, sizeof(cpkt));
}

/* Functions for use inside qemu to open and read from/write to ports */
int virtio_serial_open(VirtIOSerialPort *port)
{
    /* Don't allow opening an already-open port */
    if (port->host_connected) {
        return 0;
    }
    /* Send port open notification to the guest */
    port->host_connected = true;
    send_control_event(port, VIRTIO_CONSOLE_PORT_OPEN, 1);

    return 0;
}

int virtio_serial_close(VirtIOSerialPort *port)
{
    port->host_connected = false;
    send_control_event(port, VIRTIO_CONSOLE_PORT_OPEN, 0);

    return 0;
}

/* Individual ports/apps call this function to write to the guest. */
ssize_t virtio_serial_write(VirtIOSerialPort *port, const uint8_t *buf,
                            size_t size)
{
    if (!port || !port->host_connected || !port->guest_connected) {
        return 0;
    }
    return write_to_port(port, buf, size);
}

/*
 * Readiness of the guest to accept data on a port.
 * Returns max. data the guest can receive
 */
size_t virtio_serial_guest_ready(VirtIOSerialPort *port)
{
    VirtQueue *vq = port->ivq;

    if (!virtio_queue_ready(vq) ||
        !(port->vser->vdev.status & VIRTIO_CONFIG_S_DRIVER_OK) ||
        virtio_queue_empty(vq)) {
        return 0;
    }
    if (use_multiport(port->vser) && !port->guest_connected) {
        return 0;
    }

    if (virtqueue_avail_bytes(vq, 4096, 0)) {
        return 4096;
    }
    if (virtqueue_avail_bytes(vq, 1, 0)) {
        return 1;
    }
    return 0;
}

/* Guest wants to notify us of some event */
static void handle_control_message(VirtIOSerial *vser, void *buf)
{
    struct VirtIOSerialPort *port;
    struct virtio_console_control cpkt, *gcpkt;
    uint8_t *buffer;
    size_t buffer_len;

    gcpkt = buf;

    cpkt.event = lduw_p(&gcpkt->event);
    cpkt.value = lduw_p(&gcpkt->value);

    port = find_port_by_id(vser, ldl_p(&gcpkt->id));
    if (!port && cpkt.event != VIRTIO_CONSOLE_DEVICE_READY)
        return;

    switch(cpkt.event) {
    case VIRTIO_CONSOLE_DEVICE_READY:
        /*
         * The device is up, we can now tell the device about all the
         * ports we have here.
         */
        QTAILQ_FOREACH(port, &vser->ports, next) {
            send_control_event(port, VIRTIO_CONSOLE_PORT_ADD, 1);
        }
        break;

    case VIRTIO_CONSOLE_PORT_READY:
        /*
         * Now that we know the guest asked for the port name, we're
         * sure the guest has initialised whatever state is necessary
         * for this port. Now's a good time to let the guest know if
         * this port is a console port so that the guest can hook it
         * up to hvc.
         */
        if (port->is_console) {
            send_control_event(port, VIRTIO_CONSOLE_CONSOLE_PORT, 1);
        }

        if (port->name) {
            stw_p(&cpkt.event, VIRTIO_CONSOLE_PORT_NAME);
            stw_p(&cpkt.value, 1);

            buffer_len = sizeof(cpkt) + strlen(port->name) + 1;
            buffer = qemu_malloc(buffer_len);

            memcpy(buffer, &cpkt, sizeof(cpkt));
            memcpy(buffer + sizeof(cpkt), port->name, strlen(port->name));
            buffer[buffer_len - 1] = 0;

            send_control_msg(port, buffer, buffer_len);
            qemu_free(buffer);
        }

        if (port->host_connected) {
            send_control_event(port, VIRTIO_CONSOLE_PORT_OPEN, 1);
        }

        /*
         * When the guest has asked us for this information it means
         * the guest is all setup and has its virtqueues
         * initialised. If some app is interested in knowing about
         * this event, let it know.
         */
        if (port->info->guest_ready) {
            port->info->guest_ready(port);
        }
        break;

    case VIRTIO_CONSOLE_PORT_OPEN:
        port->guest_connected = cpkt.value;
        if (cpkt.value && port->info->guest_open) {
            /* Send the guest opened notification if an app is interested */
            port->info->guest_open(port);
        }

        if (!cpkt.value && port->info->guest_close) {
            /* Send the guest closed notification if an app is interested */
            port->info->guest_close(port);
        }
        break;
    }
}

static void control_in(VirtIODevice *vdev, VirtQueue *vq)
{
}

static void control_out(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtQueueElement elem;
    VirtIOSerial *vser;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);

    while (virtqueue_pop(vq, &elem)) {
        handle_control_message(vser, elem.out_sg[0].iov_base);
        virtqueue_push(vq, &elem, elem.out_sg[0].iov_len);
    }
    virtio_notify(vdev, vq);
}

/* Guest wrote something to some port. */
static void handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSerial *vser;
    VirtQueueElement elem;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);

    while (virtqueue_pop(vq, &elem)) {
        VirtIOSerialPort *port;
        size_t ret;

        port = find_port_by_vq(vser, vq);
        if (!port) {
            ret = 0;
            goto next_buf;
        }

        /*
         * A port may not have any handler registered for consuming the
         * data that the guest sends or it may not have a chardev associated
         * with it. Just ignore the data in that case.
         */
        if (!port->info->have_data) {
            ret = 0;
            goto next_buf;
        }

        /* The guest always sends only one sg */
        ret = port->info->have_data(port, elem.out_sg[0].iov_base,
                                    elem.out_sg[0].iov_len);

    next_buf:
        virtqueue_push(vq, &elem, ret);
    }
    virtio_notify(vdev, vq);
}

static void handle_input(VirtIODevice *vdev, VirtQueue *vq)
{
}

static uint32_t get_features(VirtIODevice *vdev, uint32_t features)
{
    VirtIOSerial *vser;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);

    if (vser->bus->max_nr_ports > 1) {
        features |= (1 << VIRTIO_CONSOLE_F_MULTIPORT);
    }
    return features;
}

/* Guest requested config info */
static void get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOSerial *vser;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);
    memcpy(config_data, &vser->config, sizeof(struct virtio_console_config));
}

static void set_config(VirtIODevice *vdev, const uint8_t *config_data)
{
    struct virtio_console_config config;

    memcpy(&config, config_data, sizeof(config));
}

static void virtio_serial_save(QEMUFile *f, void *opaque)
{
    VirtIOSerial *s = opaque;
    VirtIOSerialPort *port;
    uint32_t nr_active_ports;
    unsigned int i;

    /* The virtio device */
    virtio_save(&s->vdev, f);

    /* The config space */
    qemu_put_be16s(f, &s->config.cols);
    qemu_put_be16s(f, &s->config.rows);

    qemu_put_be32s(f, &s->config.max_nr_ports);

    /* The ports map */

    for (i = 0; i < (s->config.max_nr_ports + 31) / 32; i++) {
        qemu_put_be32s(f, &s->ports_map[i]);
    }

    /* Ports */

    nr_active_ports = 0;
    QTAILQ_FOREACH(port, &s->ports, next) {
        nr_active_ports++;
    }

    qemu_put_be32s(f, &nr_active_ports);

    /*
     * Items in struct VirtIOSerialPort.
     */
    QTAILQ_FOREACH(port, &s->ports, next) {
        qemu_put_be32s(f, &port->id);
        qemu_put_byte(f, port->guest_connected);
        qemu_put_byte(f, port->host_connected);
    }
}

static int virtio_serial_load(QEMUFile *f, void *opaque, int version_id)
{
    VirtIOSerial *s = opaque;
    VirtIOSerialPort *port;
    size_t ports_map_size;
    uint32_t max_nr_ports, nr_active_ports, *ports_map;
    unsigned int i;

    if (version_id > 2) {
        return -EINVAL;
    }

    /* The virtio device */
    virtio_load(&s->vdev, f);

    if (version_id < 2) {
        return 0;
    }

    /* The config space */
    qemu_get_be16s(f, &s->config.cols);
    qemu_get_be16s(f, &s->config.rows);

    qemu_get_be32s(f, &max_nr_ports);
    if (max_nr_ports > s->config.max_nr_ports) {
        /* Source could have had more ports than us. Fail migration. */
        return -EINVAL;
    }

    ports_map_size = sizeof(uint32_t) * (max_nr_ports + 31) / 32;
    ports_map = qemu_malloc(ports_map_size);

    for (i = 0; i < (max_nr_ports + 31) / 32; i++) {
        qemu_get_be32s(f, &ports_map[i]);

        if (ports_map[i] != s->ports_map[i]) {
            /*
             * Ports active on source and destination don't
             * match. Fail migration.
             */
            qemu_free(ports_map);
            return -EINVAL;
        }
    }
    qemu_free(ports_map);

    qemu_get_be32s(f, &nr_active_ports);

    /* Items in struct VirtIOSerialPort */
    for (i = 0; i < nr_active_ports; i++) {
        uint32_t id;
        bool host_connected;

        id = qemu_get_be32(f);
        port = find_port_by_id(s, id);

        port->guest_connected = qemu_get_byte(f);
        host_connected = qemu_get_byte(f);
        if (host_connected != port->host_connected) {
            /*
             * We have to let the guest know of the host connection
             * status change
             */
            send_control_event(port, VIRTIO_CONSOLE_PORT_OPEN,
                               port->host_connected);
        }
    }
    return 0;
}

static void virtser_bus_dev_print(Monitor *mon, DeviceState *qdev, int indent);

static struct BusInfo virtser_bus_info = {
    .name      = "virtio-serial-bus",
    .size      = sizeof(VirtIOSerialBus),
    .print_dev = virtser_bus_dev_print,
};

static VirtIOSerialBus *virtser_bus_new(DeviceState *dev)
{
    VirtIOSerialBus *bus;

    bus = FROM_QBUS(VirtIOSerialBus, qbus_create(&virtser_bus_info, dev, NULL));
    bus->qbus.allow_hotplug = 1;

    return bus;
}

static void virtser_bus_dev_print(Monitor *mon, DeviceState *qdev, int indent)
{
    VirtIOSerialDevice *dev = DO_UPCAST(VirtIOSerialDevice, qdev, qdev);
    VirtIOSerialPort *port = DO_UPCAST(VirtIOSerialPort, dev, &dev->qdev);

    monitor_printf(mon, "%*s dev-prop-int: id: %u\n",
                   indent, "", port->id);
    monitor_printf(mon, "%*s dev-prop-int: guest_connected: %d\n",
                   indent, "", port->guest_connected);
    monitor_printf(mon, "%*s dev-prop-int: host_connected: %d\n",
                   indent, "", port->host_connected);
}

/* This function is only used if a port id is not provided by the user */
static uint32_t find_free_port_id(VirtIOSerial *vser)
{
    unsigned int i;

    for (i = 0; i < (vser->config.max_nr_ports + 31) / 32; i++) {
        uint32_t map, bit;

        map = vser->ports_map[i];
        bit = ffs(~map);
        if (bit) {
            return (bit - 1) + i * 32;
        }
    }
    return VIRTIO_CONSOLE_BAD_ID;
}

static void mark_port_added(VirtIOSerial *vser, uint32_t port_id)
{
    unsigned int i;

    i = port_id / 32;
    vser->ports_map[i] |= 1U << (port_id % 32);
}

static void add_port(VirtIOSerial *vser, uint32_t port_id)
{
    mark_port_added(vser, port_id);

    send_control_event(find_port_by_id(vser, port_id),
                       VIRTIO_CONSOLE_PORT_ADD, 1);
}

static void remove_port(VirtIOSerial *vser, uint32_t port_id)
{
    unsigned int i;

    i = port_id / 32;
    vser->ports_map[i] &= ~(1U << (port_id % 32));

    send_control_event(find_port_by_id(vser, port_id),
                       VIRTIO_CONSOLE_PORT_REMOVE, 1);
}

static int virtser_port_qdev_init(DeviceState *qdev, DeviceInfo *base)
{
    VirtIOSerialDevice *dev = DO_UPCAST(VirtIOSerialDevice, qdev, qdev);
    VirtIOSerialPortInfo *info = DO_UPCAST(VirtIOSerialPortInfo, qdev, base);
    VirtIOSerialPort *port = DO_UPCAST(VirtIOSerialPort, dev, &dev->qdev);
    VirtIOSerialBus *bus = DO_UPCAST(VirtIOSerialBus, qbus, qdev->parent_bus);
    int ret;
    bool plugging_port0;

    port->vser = bus->vser;

    /*
     * Is the first console port we're seeing? If so, put it up at
     * location 0. This is done for backward compatibility (old
     * kernel, new qemu).
     */
    plugging_port0 = port->is_console && !find_port_by_id(port->vser, 0);

    if (find_port_by_id(port->vser, port->id)) {
        error_report("virtio-serial-bus: A port already exists at id %u\n",
                     port->id);
        return -1;
    }

    if (port->id == VIRTIO_CONSOLE_BAD_ID) {
        if (plugging_port0) {
            port->id = 0;
        } else {
            port->id = find_free_port_id(port->vser);
            if (port->id == VIRTIO_CONSOLE_BAD_ID) {
                error_report("virtio-serial-bus: Maximum port limit for this device reached\n");
                return -1;
            }
        }
    }

    if (port->id >= port->vser->config.max_nr_ports) {
        error_report("virtio-serial-bus: Out-of-range port id specified, max. allowed: %u\n",
                     port->vser->config.max_nr_ports - 1);
        return -1;
    }

    dev->info = info;
    ret = info->init(dev);
    if (ret) {
        return ret;
    }

    if (!use_multiport(port->vser)) {
        /*
         * Allow writes to guest in this case; we have no way of
         * knowing if a guest port is connected.
         */
        port->guest_connected = true;
    }

    QTAILQ_INSERT_TAIL(&port->vser->ports, port, next);
    port->ivq = port->vser->ivqs[port->id];
    port->ovq = port->vser->ovqs[port->id];

    add_port(port->vser, port->id);

    /* Send an update to the guest about this new port added */
    virtio_notify_config(&port->vser->vdev);

    return ret;
}

static int virtser_port_qdev_exit(DeviceState *qdev)
{
    VirtIOSerialDevice *dev = DO_UPCAST(VirtIOSerialDevice, qdev, qdev);
    VirtIOSerialPort *port = DO_UPCAST(VirtIOSerialPort, dev, &dev->qdev);
    VirtIOSerial *vser = port->vser;

    remove_port(port->vser, port->id);

    QTAILQ_REMOVE(&vser->ports, port, next);

    if (port->info->exit)
        port->info->exit(dev);

    return 0;
}

void virtio_serial_port_qdev_register(VirtIOSerialPortInfo *info)
{
    info->qdev.init = virtser_port_qdev_init;
    info->qdev.bus_info = &virtser_bus_info;
    info->qdev.exit = virtser_port_qdev_exit;
    info->qdev.unplug = qdev_simple_unplug_cb;
    qdev_register(&info->qdev);
}

VirtIODevice *virtio_serial_init(DeviceState *dev, uint32_t max_nr_ports)
{
    VirtIOSerial *vser;
    VirtIODevice *vdev;
    uint32_t i;

    if (!max_nr_ports)
        return NULL;

    vdev = virtio_common_init("virtio-serial", VIRTIO_ID_CONSOLE,
                              sizeof(struct virtio_console_config),
                              sizeof(VirtIOSerial));

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);

    /* Spawn a new virtio-serial bus on which the ports will ride as devices */
    vser->bus = virtser_bus_new(dev);
    vser->bus->vser = vser;
    QTAILQ_INIT(&vser->ports);

    vser->bus->max_nr_ports = max_nr_ports;
    vser->ivqs = qemu_malloc(max_nr_ports * sizeof(VirtQueue *));
    vser->ovqs = qemu_malloc(max_nr_ports * sizeof(VirtQueue *));

    /* Add a queue for host to guest transfers for port 0 (backward compat) */
    vser->ivqs[0] = virtio_add_queue(vdev, 128, handle_input);
    /* Add a queue for guest to host transfers for port 0 (backward compat) */
    vser->ovqs[0] = virtio_add_queue(vdev, 128, handle_output);

    /* control queue: host to guest */
    vser->c_ivq = virtio_add_queue(vdev, 16, control_in);
    /* control queue: guest to host */
    vser->c_ovq = virtio_add_queue(vdev, 16, control_out);

    for (i = 1; i < vser->bus->max_nr_ports; i++) {
        /* Add a per-port queue for host to guest transfers */
        vser->ivqs[i] = virtio_add_queue(vdev, 128, handle_input);
        /* Add a per-per queue for guest to host transfers */
        vser->ovqs[i] = virtio_add_queue(vdev, 128, handle_output);
    }

    vser->config.max_nr_ports = max_nr_ports;
    vser->ports_map = qemu_mallocz((max_nr_ports + 31) / 32);
    /*
     * Reserve location 0 for a console port for backward compat
     * (old kernel, new qemu)
     */
    mark_port_added(vser, 0);

    vser->vdev.get_features = get_features;
    vser->vdev.get_config = get_config;
    vser->vdev.set_config = set_config;

    /*
     * Register for the savevm section with the virtio-console name
     * to preserve backward compat
     */
    register_savevm("virtio-console", -1, 2, virtio_serial_save,
                    virtio_serial_load, vser);

    return vdev;
}
