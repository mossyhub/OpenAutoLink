/*
 * libusb stub — satisfies aasdk linker without pulling in real USB support.
 * We use Nearby Connections transport on Android, not USB.
 */
#include "libusb.h"

int libusb_init(libusb_context **ctx) { (void)ctx; return -1; }
void libusb_exit(libusb_context *ctx) { (void)ctx; }
ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list) {
    (void)ctx; (void)list; return 0;
}
void libusb_free_device_list(libusb_device **list, int unref_devices) {
    (void)list; (void)unref_devices;
}
int libusb_open(libusb_device *dev, libusb_device_handle **handle) {
    (void)dev; (void)handle; return -1;
}
void libusb_close(libusb_device_handle *handle) { (void)handle; }
int libusb_claim_interface(libusb_device_handle *handle, int iface) {
    (void)handle; (void)iface; return -1;
}
int libusb_release_interface(libusb_device_handle *handle, int iface) {
    (void)handle; (void)iface; return -1;
}
int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc) {
    (void)dev; (void)desc; return -1;
}
int libusb_get_config_descriptor(libusb_device *dev, uint8_t config_index, struct libusb_config_descriptor **config) {
    (void)dev; (void)config_index; (void)config; return -1;
}
void libusb_free_config_descriptor(struct libusb_config_descriptor *config) { (void)config; }
int libusb_control_transfer(libusb_device_handle *handle, uint8_t bmRequestType,
    uint8_t bRequest, uint16_t wValue, uint16_t wIndex, unsigned char *data,
    uint16_t wLength, unsigned int timeout) {
    (void)handle; (void)bmRequestType; (void)bRequest; (void)wValue;
    (void)wIndex; (void)data; (void)wLength; (void)timeout;
    return -1;
}
int libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags,
    int vendor_id, int product_id, int dev_class,
    void *cb_fn, void *user_data, void *handle) {
    (void)ctx; (void)events; (void)flags; (void)vendor_id;
    (void)product_id; (void)dev_class; (void)cb_fn; (void)user_data; (void)handle;
    return -1;
}
void libusb_hotplug_deregister_callback(libusb_context *ctx, int handle) {
    (void)ctx; (void)handle;
}
int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed) {
    (void)ctx; (void)tv; (void)completed; return -1;
}
