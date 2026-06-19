#ifndef TYPES_H
#define TYPES_H

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;

typedef uint32_t            size_t;
typedef uint32_t            ptrdiff_t;

typedef uint32_t            physaddr_t;
typedef uint32_t            virtaddr_t;

#define NULL                ((void*)0)

#endif