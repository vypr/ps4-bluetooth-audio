#ifndef BT_AUDIO_PROTOCOL_H
#define BT_AUDIO_PROTOCOL_H
#define BT_DEVICE_LIMIT 32
struct bt_device { unsigned long address; unsigned cod,services,flags,reserved; char name[96]; };
struct bt_devices { unsigned count,reserved; struct bt_device items[BT_DEVICE_LIMIT]; };
/* What the headphone link is really doing. A UI must show this, not guess it
 * from "selected" or "wanted": a selected device may be offline or retrying. */
#define BT_LINK_IDLE 0       /* nothing requested */
#define BT_LINK_CONNECTING 1 /* attempt in progress */
#define BT_LINK_STREAMING 2  /* audio is flowing to the headphones */
#define BT_LINK_LOST 3       /* was streaming, link dropped, reconnecting */
#define BT_LINK_FAILED 4     /* last attempt failed, waiting to retry */
#define BT_LINK_SEARCHING 5  /* scanning for the selected headphones */
static inline int bt_is_audio(const struct bt_device*d){return (d->services&8)||(((d->cod>>8)&31)==4&&!(d->services&0x40));}
int bt_list(struct bt_devices*);
int bt_prepare(unsigned long address);
#endif
