/*
 * Minimal libusb-1.0 header stub — just enough types/functions for aasdk to link.
 * Real USB is not used on Android (we use Nearby Connections transport).
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

struct libusb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

struct libusb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
};

struct libusb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
    const struct libusb_endpoint_descriptor *endpoint;
};

struct libusb_interface {
    const struct libusb_interface_descriptor *altsetting;
    int num_altsetting;
};

struct libusb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  MaxPower;
    const struct libusb_interface *interface;
    const unsigned char *extra;
    int extra_length;
};

int libusb_init(libusb_context **ctx);
void libusb_exit(libusb_context *ctx);
ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list);
void libusb_free_device_list(libusb_device **list, int unref_devices);
int libusb_open(libusb_device *dev, libusb_device_handle **handle);
void libusb_close(libusb_device_handle *handle);
int libusb_claim_interface(libusb_device_handle *handle, int iface);
int libusb_release_interface(libusb_device_handle *handle, int iface);
int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc);
int libusb_get_config_descriptor(libusb_device *dev, uint8_t config_index, struct libusb_config_descriptor **config);
void libusb_free_config_descriptor(struct libusb_config_descriptor *config);
int libusb_control_transfer(libusb_device_handle *handle, uint8_t bmRequestType,
    uint8_t bRequest, uint16_t wValue, uint16_t wIndex, unsigned char *data,
    uint16_t wLength, unsigned int timeout);
int libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags,
    int vendor_id, int product_id, int dev_class,
    void *cb_fn, void *user_data, void *handle);
void libusb_hotplug_deregister_callback(libusb_context *ctx, int handle);
int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed);

#ifdef __cplusplus
}
#endif
