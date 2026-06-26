#include "drivers/device.h"
#include "lib/string.h"
#include "mm/memory.h"

static device_t* device_list = NULL;
static int device_count = 0;
static int next_device_id = 1;

void device_init() {
    device_list = NULL;
    device_count = 0;
    next_device_id = 1;
}

int device_register(device_t* dev) {
    if (device_count >= MAX_DEVICES) return -1;
    if (dev == NULL) return -1;
    
    dev->id = next_device_id++;
    dev->ref_count = 0;
    dev->next = device_list;
    device_list = dev;
    device_count++;
    
    return dev->id;
}

device_t* device_find(const char* name) {
    device_t* dev = device_list;
    while (dev != NULL) {
        if (strcmp(dev->name, name) == 0) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

device_t* device_find_by_type(device_type_t type) {
    device_t* dev = device_list;
    while (dev != NULL) {
        if (dev->type == type) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

int device_open(const char* name) {
    device_t* dev = device_find(name);
    if (dev == NULL) return -1;
    
    if (dev->open != NULL) {
        int ret = dev->open(dev);
        if (ret != 0) return ret;
    }
    
    dev->ref_count++;
    return dev->id;
}

int device_close(device_t* dev) {
    if (dev == NULL) return -1;
    
    if (dev->ref_count > 0) {
        dev->ref_count--;
    }
    
    if (dev->close != NULL) {
        return dev->close(dev);
    }
    
    return 0;
}

int device_read(device_t* dev, void* buffer, size_t size) {
    if (dev == NULL || buffer == NULL) return -1;
    if (dev->read == NULL) return -1;
    
    return dev->read(dev, buffer, size);
}

int device_write(device_t* dev, const void* buffer, size_t size) {
    if (dev == NULL || buffer == NULL) return -1;
    if (dev->write == NULL) return -1;
    
    return dev->write(dev, buffer, size);
}

int device_ioctl(device_t* dev, int cmd, void* arg) {
    if (dev == NULL) return -1;
    if (dev->ioctl == NULL) return -1;
    
    return dev->ioctl(dev, cmd, arg);
}