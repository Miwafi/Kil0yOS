#ifndef DEVICE_H
#define DEVICE_H

#include "lib/types.h"

#define MAX_DEVICES 32
#define MAX_DEVICE_NAME 32

typedef enum {
    DEVICE_TYPE_NONE,
    DEVICE_TYPE_BLOCK,
    DEVICE_TYPE_CHAR,
    DEVICE_TYPE_NETWORK,
    DEVICE_TYPE_KEYBOARD,
    DEVICE_TYPE_MOUSE,
    DEVICE_TYPE_DISK,
    DEVICE_TYPE_TERMINAL
} device_type_t;

typedef struct device device_t;

struct device {
    char name[MAX_DEVICE_NAME];
    device_type_t type;
    int id;
    int ref_count;
    
    int (*open)(device_t* dev);
    int (*close)(device_t* dev);
    int (*read)(device_t* dev, void* buffer, size_t size);
    int (*write)(device_t* dev, const void* buffer, size_t size);
    int (*ioctl)(device_t* dev, int cmd, void* arg);
    
    void* private_data;
    device_t* next;
};

void device_init();
int device_register(device_t* dev);
device_t* device_find(const char* name);
device_t* device_find_by_type(device_type_t type);
int device_open(const char* name);
int device_close(device_t* dev);
int device_read(device_t* dev, void* buffer, size_t size);
int device_write(device_t* dev, const void* buffer, size_t size);
int device_ioctl(device_t* dev, int cmd, void* arg);

#endif