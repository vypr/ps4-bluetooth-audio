#ifndef BTA_TYPES_H
#define BTA_TYPES_H
/* The fixed-width integer names used across the service. Kept in one guarded
 * header so the payload sources and the host unit tests agree on them without
 * pulling in any platform headers. */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#endif
