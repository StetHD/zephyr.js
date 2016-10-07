// Copyright (c) 2016, Intel Corporation.
#ifdef BUILD_MODULE_BLE
#ifndef QEMU_BUILD
// Zephyr includes
#include <zephyr.h>
#include <string.h>
#include <stdlib.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

// ZJS includes
#include "zjs_ble.h"
#include "zjs_buffer.h"
#include "zjs_util.h"

#define ZJS_BLE_UUID_LEN                            36

#define ZJS_BLE_RESULT_SUCCESS                      0x00
#define ZJS_BLE_RESULT_INVALID_OFFSET               BT_ATT_ERR_INVALID_OFFSET
#define ZJS_BLE_RESULT_ATTR_NOT_LONG                BT_ATT_ERR_ATTRIBUTE_NOT_LONG
#define ZJS_BLE_RESULT_INVALID_ATTRIBUTE_LENGTH     BT_ATT_ERR_INVALID_ATTRIBUTE_LEN
#define ZJS_BLE_RESULT_UNLIKELY_ERROR               BT_ATT_ERR_UNLIKELY

#define ZJS_BLE_TIMEOUT_TICKS                       500

struct nano_sem zjs_ble_nano_sem;

static struct bt_gatt_ccc_cfg zjs_ble_blvl_ccc_cfg[CONFIG_BLUETOOTH_MAX_PAIRED] = {};
static uint8_t zjs_ble_simulate_blvl;

struct bt_conn *zjs_ble_default_conn;

struct zjs_ble_read_callback {
    struct zjs_callback zjs_cb;
    uint16_t offset;                        // arg
    uint32_t error_code;                    // return value
    const void *buffer;                     // return value
    ssize_t buffer_size;                    // return value
};

struct zjs_ble_write_callback {
    struct zjs_callback zjs_cb;
    const void *buffer;                     // arg
    uint16_t buffer_size;                   // arg
    uint16_t offset;                        // arg
    uint32_t error_code;                    // return value
};

struct zjs_ble_subscribe_callback {
    struct zjs_callback zjs_cb;
    uint16_t max_value_size;
};

struct zjs_ble_unsubscribe_callback {
    struct zjs_callback zjs_cb;
    // placeholders args
};

struct zjs_ble_notify_callback {
    struct zjs_callback zjs_cb;
    // placeholders args
};

struct zjs_ble_characteristic {
    int flags;
    jerry_value_t chrc_obj;
    struct bt_uuid *uuid;
    struct bt_gatt_attr *chrc_attr;
    jerry_value_t cud_value;
    struct zjs_ble_read_callback read_cb;
    struct zjs_ble_write_callback write_cb;
    struct zjs_ble_subscribe_callback subscribe_cb;
    struct zjs_ble_unsubscribe_callback unsubscribe_cb;
    struct zjs_ble_notify_callback notify_cb;
    struct zjs_ble_characteristic *next;
};

struct zjs_ble_service {
    jerry_value_t service_obj;
    struct bt_uuid *uuid;
    struct zjs_ble_characteristic *characteristics;
    struct zjs_ble_service *next;
};

#define MAX_TYPE_LEN 20

struct zjs_ble_list_item {
    char event_type[MAX_TYPE_LEN];  // null-terminated
    struct zjs_callback zjs_cb;
    uint32_t intdata;
    struct zjs_ble_list_item *next;
};

static struct zjs_ble_service *zjs_ble_services = NULL;
static struct zjs_ble_list_item *zjs_ble_list = NULL;

struct bt_uuid* zjs_ble_new_uuid_16(uint16_t value) {
    struct bt_uuid_16* uuid = zjs_malloc(sizeof(struct bt_uuid_16));
    if (!uuid) {
        PRINT("zjs_ble_new_uuid_16: out of memory allocating struct bt_uuid_16\n");
        return NULL;
    }

    memset(uuid, 0, sizeof(struct bt_uuid_16));

    uuid->uuid.type = BT_UUID_TYPE_16;
    uuid->val = value;
    return (struct bt_uuid *) uuid;
}

static void zjs_ble_free_characteristics(struct zjs_ble_characteristic *chrc)
{
    struct zjs_ble_characteristic *tmp;
    while (chrc != NULL) {
        tmp = chrc;
        chrc = chrc->next;

        jerry_release_value(tmp->chrc_obj);

        if (tmp->uuid)
            zjs_free(tmp->uuid);
        if (tmp->read_cb.zjs_cb.js_callback)
            jerry_release_value(tmp->read_cb.zjs_cb.js_callback);
        if (tmp->write_cb.zjs_cb.js_callback)
            jerry_release_value(tmp->write_cb.zjs_cb.js_callback);
        if (tmp->subscribe_cb.zjs_cb.js_callback)
            jerry_release_value(tmp->subscribe_cb.zjs_cb.js_callback);
        if (tmp->unsubscribe_cb.zjs_cb.js_callback)
            jerry_release_value(tmp->unsubscribe_cb.zjs_cb.js_callback);
        if (tmp->notify_cb.zjs_cb.js_callback)
            jerry_release_value(tmp->notify_cb.zjs_cb.js_callback);

        zjs_free(tmp);
    }
}

static void zjs_ble_free_services(struct zjs_ble_service *service)
{
    struct zjs_ble_service *tmp;
    while (service != NULL) {
        tmp = service;
        service = service->next;

        jerry_release_value(tmp->service_obj);

        if (tmp->uuid)
            zjs_free(tmp->uuid);
        if (tmp->characteristics)
            zjs_ble_free_characteristics(tmp->characteristics);

        zjs_free(tmp);
    }
}

static struct zjs_ble_list_item *zjs_ble_event_callback_alloc()
{
    // effects: allocates a new callback list item and adds it to the list
    struct zjs_ble_list_item *item;
    item = zjs_malloc(sizeof(struct zjs_ble_list_item));
    if (!item) {
        PRINT("zjs_ble_event_callback_alloc: out of memory allocating callback struct\n");
        return NULL;
    }

    item->next = zjs_ble_list;
    zjs_ble_list = item;
    return item;
}

static void zjs_ble_queue_dispatch(char *type, zjs_cb_wrapper_t func,
                                   uint32_t intdata)
{
    // requires: called only from task context, type is the string event type
    //             and at most MAX_TYPE_LEN (20) chars,  func is a function
    //             that can handle calling the callback for this event type
    //             when found, intdata is a uint32 that will be stored in the
    //             appropriate callback struct for use by func (just set it to
    //             0 if not needed)
    //  effects: finds the first callback for the given type and queues it up
    //             to run func to execute it at the next opportunity
    struct zjs_ble_list_item *ev = zjs_ble_list;
    while (ev) {
        if (!strncmp(ev->event_type, type, MAX_TYPE_LEN)) {
            ev->zjs_cb.call_function = func;
            ev->intdata = intdata;
            zjs_queue_callback(&ev->zjs_cb);
            return;
        }
        ev = ev->next;
    }
}

static jerry_value_t zjs_ble_read_attr_call_function_return(const jerry_value_t function_obj,
                                                            const jerry_value_t this,
                                                            const jerry_value_t argv[],
                                                            const jerry_length_t argc)
{
    if (argc != 2 ||
        !jerry_value_is_number(argv[0]) ||
        !jerry_value_is_object(argv[1])) {
        nano_task_sem_give(&zjs_ble_nano_sem);
        return zjs_error("zjs_ble_read_attr_call_function_return: invalid arguments");
    }

    uintptr_t ptr;
    if (jerry_get_object_native_handle(function_obj, &ptr)) {
        // store the return value in the read_cb struct
        struct zjs_ble_characteristic *chrc = (struct zjs_ble_characteristic*)ptr;
        chrc->read_cb.error_code = (uint32_t)jerry_get_number_value(argv[0]);


        zjs_buffer_t *buf = zjs_buffer_find(argv[1]);
        if (buf) {
            chrc->read_cb.buffer = buf->buffer;
            chrc->read_cb.buffer_size = buf->bufsize;
        } else {
            PRINT("zjs_ble_read_attr_call_function_return: buffer not found\n");
        }
    }

    // unblock fiber
    nano_task_sem_give(&zjs_ble_nano_sem);
    return ZJS_UNDEFINED;
}

static void zjs_ble_read_attr_call_function(struct zjs_callback *cb)
{
    struct zjs_ble_read_callback *mycb;
    struct zjs_ble_characteristic *chrc;
    mycb = CONTAINER_OF(cb, struct zjs_ble_read_callback, zjs_cb);
    chrc = CONTAINER_OF(mycb, struct zjs_ble_characteristic, read_cb);

    jerry_value_t rval;
    jerry_value_t args[2];
    jerry_value_t func_obj;

    args[0] = jerry_create_number(mycb->offset);
    func_obj = jerry_create_external_function(zjs_ble_read_attr_call_function_return);
    jerry_set_object_native_handle(func_obj, (uintptr_t)chrc, NULL);
    args[1] = func_obj;

    rval = jerry_call_function(mycb->zjs_cb.js_callback, chrc->chrc_obj, args, 2);
    if (jerry_value_has_error_flag(rval)) {
        PRINT("zjs_ble_read_attr_call_function: failed to call onReadRequest function\n");
    }

    jerry_release_value(args[0]);
    jerry_release_value(args[1]);
    jerry_release_value(rval);
}

static ssize_t zjs_ble_read_attr_callback(struct bt_conn *conn,
                                          const struct bt_gatt_attr *attr,
                                          void *buf, uint16_t len,
                                          uint16_t offset)
{
    if (offset > len) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    struct zjs_ble_characteristic* chrc = attr->user_data;

    if (!chrc) {
        PRINT("zjs_ble_read_attr_callback: characteristic not found\n");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    if (chrc->read_cb.zjs_cb.js_callback) {
        // This is from the FIBER context, so we queue up the callback
        // to invoke js from task context
        chrc->read_cb.offset = offset;
        chrc->read_cb.buffer = NULL;
        chrc->read_cb.buffer_size = 0;
        chrc->read_cb.error_code = BT_ATT_ERR_NOT_SUPPORTED;
        chrc->read_cb.zjs_cb.call_function = zjs_ble_read_attr_call_function;
        zjs_queue_callback(&chrc->read_cb.zjs_cb);

        // block until result is ready
        if (!nano_fiber_sem_take(&zjs_ble_nano_sem, ZJS_BLE_TIMEOUT_TICKS)) {
            PRINT("zjs_ble_read_attr_callback: JS callback timed out\n");
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }

        if (chrc->read_cb.error_code == ZJS_BLE_RESULT_SUCCESS) {
            if (chrc->read_cb.buffer && chrc->read_cb.buffer_size > 0) {
                // buffer should be pointing to the Buffer object that JS created
                // copy the bytes into the return buffer ptr
                memcpy(buf, chrc->read_cb.buffer, chrc->read_cb.buffer_size);
                return chrc->read_cb.buffer_size;
            }

            PRINT("zjs_ble_read_attr_callback: buffer is empty\n");
            return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
        } else {
            PRINT("zjs_ble_read_attr_callback: on read attr error %lu\n", chrc->read_cb.error_code);
            return BT_GATT_ERR(chrc->read_cb.error_code);
        }
    }

    PRINT("zjs_ble_read_attr_callback: js callback not available\n");
    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
}

static jerry_value_t zjs_ble_write_attr_call_function_return(const jerry_value_t function_obj,
                                                             const jerry_value_t this,
                                                             const jerry_value_t argv[],
                                                             const jerry_length_t argc)
{
    if (argc != 1 ||
        !jerry_value_is_number(argv[0])) {
        nano_task_sem_give(&zjs_ble_nano_sem);
        return zjs_error("zjs_ble_write_attr_call_function_return: invalid arguments");
    }

    uintptr_t ptr;
    if (jerry_get_object_native_handle(function_obj, &ptr)) {
        // store the return value in the write_cb struct
        struct zjs_ble_characteristic *chrc = (struct zjs_ble_characteristic*)ptr;
        chrc->write_cb.error_code = (uint32_t)jerry_get_number_value(argv[0]);
    }

    // unblock fiber
    nano_task_sem_give(&zjs_ble_nano_sem);
    return ZJS_UNDEFINED;
}

static void zjs_ble_write_attr_call_function(struct zjs_callback *cb)
{
    struct zjs_ble_write_callback *mycb;
    struct zjs_ble_characteristic *chrc;
    mycb = CONTAINER_OF(cb, struct zjs_ble_write_callback, zjs_cb);
    chrc = CONTAINER_OF(mycb, struct zjs_ble_characteristic, write_cb);

    jerry_value_t rval;
    jerry_value_t args[4];
    jerry_value_t func_obj;

    if (mycb->buffer && mycb->buffer_size > 0) {
        jerry_value_t buf_obj = zjs_buffer_create(mycb->buffer_size);

        if (buf_obj) {
           zjs_buffer_t *buf = zjs_buffer_find(buf_obj);

           if (buf &&
               buf->buffer &&
               buf->bufsize == mycb->buffer_size) {
               memcpy(buf->buffer, mycb->buffer, mycb->buffer_size);
               args[0]= buf_obj;
           } else {
               args[0] = jerry_create_null();
           }
        } else {
            args[0] = jerry_create_null();
        }
    } else {
        args[0] = jerry_create_null();
    }

    args[1] = jerry_create_number(mycb->offset);
    args[2] = jerry_create_boolean(false);
    func_obj = jerry_create_external_function(zjs_ble_write_attr_call_function_return);
    jerry_set_object_native_handle(func_obj, (uintptr_t)chrc, NULL);
    args[3] = func_obj;

    rval = jerry_call_function(mycb->zjs_cb.js_callback, chrc->chrc_obj, args, 4);
    if (jerry_value_has_error_flag(rval)) {
        PRINT("zjs_ble_write_attr_call_function: failed to call onWriteRequest function\n");
    }

    jerry_release_value(args[0]);
    jerry_release_value(args[1]);
    jerry_release_value(args[2]);
    jerry_release_value(args[3]);
    jerry_release_value(rval);
}

static ssize_t zjs_ble_write_attr_callback(struct bt_conn *conn,
                                           const struct bt_gatt_attr *attr,
                                           const void *buf, uint16_t len,
                                           uint16_t offset, uint8_t flags)
{
    struct zjs_ble_characteristic* chrc = attr->user_data;

    if (!chrc) {
        PRINT("zjs_ble_write_attr_callback: characteristic not found\n");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    if (chrc->write_cb.zjs_cb.js_callback) {
        // This is from the FIBER context, so we queue up the callback
        // to invoke js from task context
        chrc->write_cb.offset = offset;
        chrc->write_cb.buffer = (len > 0) ? buf : NULL;
        chrc->write_cb.buffer_size = len;
        chrc->write_cb.error_code = BT_ATT_ERR_NOT_SUPPORTED;
        chrc->write_cb.zjs_cb.call_function = zjs_ble_write_attr_call_function;
        zjs_queue_callback(&chrc->write_cb.zjs_cb);

        // block until result is ready
        if (!nano_fiber_sem_take(&zjs_ble_nano_sem, ZJS_BLE_TIMEOUT_TICKS)) {
            PRINT("zjs_ble_write_attr_callback: JS callback timed out\n");
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }

        if (chrc->write_cb.error_code == ZJS_BLE_RESULT_SUCCESS) {
            return len;
        } else {
            return BT_GATT_ERR(chrc->write_cb.error_code);
        }
    }

    PRINT("zjs_ble_write_attr_callback: js callback not available\n");
    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
}

static jerry_value_t zjs_ble_update_value_call_function(const jerry_value_t function_obj,
                                                        const jerry_value_t this,
                                                        const jerry_value_t argv[],
                                                        const jerry_length_t argc)
{
    if (argc != 1 ||
        !jerry_value_is_object(argv[0])) {
        return zjs_error("zjs_ble_update_value_call_function: invalid arguments");
    }

    // expects a Buffer object
    zjs_buffer_t *buf = zjs_buffer_find(argv[0]);

    if (buf) {
        if (zjs_ble_default_conn) {
            uintptr_t ptr;
            if (jerry_get_object_native_handle(this, &ptr)) {
               struct zjs_ble_characteristic *chrc = (struct zjs_ble_characteristic*)ptr;
               if (chrc->chrc_attr) {
                   bt_gatt_notify(zjs_ble_default_conn, chrc->chrc_attr, buf->buffer, buf->bufsize);
               }
            }
        }

        return ZJS_UNDEFINED;
    }

    return zjs_error("updateValueCallback: buffer not found or empty");
}

static void zjs_ble_subscribe_call_function(struct zjs_callback *cb)
{
    struct zjs_ble_subscribe_callback *mycb = CONTAINER_OF(cb,
                                                           struct zjs_ble_subscribe_callback,
                                                           zjs_cb);
    struct zjs_ble_characteristic *chrc = CONTAINER_OF(mycb,
                                                       struct zjs_ble_characteristic,
                                                       subscribe_cb);

    jerry_value_t rval;
    jerry_value_t args[2];

    args[0] = jerry_create_number(20); // max payload size
    args[1] = jerry_create_external_function(zjs_ble_update_value_call_function);
    rval = jerry_call_function(mycb->zjs_cb.js_callback, chrc->chrc_obj, args, 2);
    if (jerry_value_has_error_flag(rval)) {
        PRINT("zjs_ble_subscribe_call_function: failed to call onSubscribe function\n");
    }

    jerry_release_value(args[0]);
    jerry_release_value(args[1]);
    jerry_release_value(rval);
}

// Port this to javascript
static void zjs_ble_blvl_ccc_cfg_changed(uint16_t value)
{
    zjs_ble_simulate_blvl = (value == BT_GATT_CCC_NOTIFY) ? 1 : 0;
}

static void zjs_ble_accept_call_function(struct zjs_callback *cb)
{
    // FIXME: get real bluetooth address
    jerry_value_t arg = jerry_create_string((jerry_char_t *)"AB:CD:DF:AB:CD:EF");
    jerry_value_t rval = jerry_call_function(cb->js_callback, ZJS_UNDEFINED, &arg, 1);
    if (jerry_value_has_error_flag(rval)) {
        PRINT("zjs_ble_accept_call_function: failed to call function\n");
    }
    jerry_release_value(rval);
    jerry_release_value(arg);
}

static void zjs_ble_disconnect_call_function(struct zjs_callback *cb)
{
    // FIXME: get real bluetooth address
    jerry_value_t arg = jerry_create_string((jerry_char_t *)"AB:CD:DF:AB:CD:EF");
    jerry_value_t rval = jerry_call_function(cb->js_callback, ZJS_UNDEFINED, &arg, 1);
    if (jerry_value_has_error_flag(rval)) {
        PRINT("zjs_ble_disconnect_call_function: failed to call function\n");
    }
    jerry_release_value(rval);
    jerry_release_value(arg);
}

static void zjs_ble_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        PRINT("zjs_ble_connected: Connection failed (err %u)\n", err);
    } else {
        DBG_PRINT("Connected\n");
        zjs_ble_default_conn = bt_conn_ref(conn);
        zjs_ble_queue_dispatch("accept", zjs_ble_accept_call_function, 0);
    }
}

static void zjs_ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
    DBG_PRINT("Disconnected (reason %u)\n", reason);

    if (zjs_ble_default_conn) {
        bt_conn_unref(zjs_ble_default_conn);
        zjs_ble_default_conn = NULL;
        zjs_ble_queue_dispatch("disconnect", zjs_ble_disconnect_call_function, 0);
    }
}

static struct bt_conn_cb zjs_ble_conn_callbacks = {
    .connected = zjs_ble_connected,
    .disconnected = zjs_ble_disconnected,
};

static void zjs_ble_auth_cancel(struct bt_conn *conn)
{
        char addr[BT_ADDR_LE_STR_LEN];

        bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

        PRINT("Pairing cancelled: %s\n", addr);
}

static struct bt_conn_auth_cb zjs_ble_auth_cb_display = {
        .cancel = zjs_ble_auth_cancel,
};

static void zjs_ble_bt_ready_call_function(struct zjs_callback *cb)
{
    // requires: called only from task context
    //  effects: handles execution of the bt ready JS callback
    jerry_value_t arg = jerry_create_string((jerry_char_t *)"poweredOn");
    jerry_value_t rval = jerry_call_function(cb->js_callback, ZJS_UNDEFINED, &arg, 1);
    if (jerry_value_has_error_flag(rval)) {
        PRINT("zjs_ble_bt_ready_call_function: failed to call function\n");
    }
    jerry_release_value(rval);
    jerry_release_value(arg);
}

static void zjs_ble_bt_ready(int err)
{
    if (!zjs_ble_list) {
        PRINT("zjs_ble_bt_ready: no event handlers present\n");
        return;
    }
    DBG_PRINT("zjs_ble_bt_ready is called [err %d]\n", err);

    // FIXME: Probably we should return this err to JS like in adv_start?
    //   Maybe this wasn't in the bleno API?
    zjs_ble_queue_dispatch("stateChange", zjs_ble_bt_ready_call_function, 0);
}

void zjs_ble_enable() {
    DBG_PRINT("About to enable the bluetooth, wait for bt_ready()...\n");
    bt_enable(zjs_ble_bt_ready);
    // setup connection callbacks
    bt_conn_cb_register(&zjs_ble_conn_callbacks);
    bt_conn_auth_cb_register(&zjs_ble_auth_cb_display);
}

static jerry_value_t zjs_ble_disconnect(const jerry_value_t function_obj,
                                        const jerry_value_t this,
                                        const jerry_value_t argv[],
                                        const jerry_length_t argc)
{
    if (zjs_ble_default_conn) {
        int error = bt_conn_disconnect(zjs_ble_default_conn,
                                       BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if (error) {
            return zjs_error("zjs_ble_disconnect: disconnect failed");
        }
    }

    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_ble_on(const jerry_value_t function_obj,
                                const jerry_value_t this,
                                const jerry_value_t argv[],
                                const jerry_length_t argc)
{
    // arg 0 should be a string event type
    // arg 1 should be a callback function
    if (argc < 2 ||
        !jerry_value_is_string(argv[0]) ||
        !jerry_value_is_object(argv[1])) {
        return zjs_error("zjs_ble_on: invalid arguments");
    }

    char event[MAX_TYPE_LEN];
    jerry_size_t sz = jerry_get_string_size(argv[0]);
    if (sz >= MAX_TYPE_LEN) {
        return zjs_error("zjs_ble_on: event type string too long");
    }
    jerry_string_to_char_buffer(argv[0], (jerry_char_t *)event, sz);
    event[sz] = '\0';

    struct zjs_ble_list_item *item = zjs_ble_event_callback_alloc();
    if (!item)
        return zjs_error("zjs_ble_on: error allocating callback");

    // TODO: we should only do this for valid event types; right now we'll
    //   store anything
    item->zjs_cb.js_callback = jerry_acquire_value(argv[1]);
    memcpy(item->event_type, event, sz + 1);

    return ZJS_UNDEFINED;
}

static void zjs_ble_adv_start_call_function(struct zjs_callback *cb)
{
    // requires: called only from task context, expects intdata in cb to have
    //             been set previously
    //  effects: handles execution of the adv start JS callback
    struct zjs_ble_list_item *mycb = CONTAINER_OF(cb, struct zjs_ble_list_item,
                                                  zjs_cb);
    jerry_value_t arg = jerry_create_number(mycb->intdata);
    jerry_value_t rval = jerry_call_function(cb->js_callback, ZJS_UNDEFINED, &arg, 1);
    if (jerry_value_has_error_flag(rval)) {
        PRINT("zjs_ble_adv_start_call_function: failed to call function\n");
    }
    jerry_release_value(arg);
    jerry_release_value(rval);
}

const int ZJS_SUCCESS = 0;
const int ZJS_URL_TOO_LONG = 1;
const int ZJS_ALLOC_FAILED = 2;
const int ZJS_URL_SCHEME_ERROR = 3;

static int zjs_encode_url_frame(jerry_value_t url, uint8_t **frame, int *size)
{
    // requires: url is a URL string, frame points to a uint8_t *, url contains
    //             only UTF-8 characters and hence no nil values
    //  effects: allocates a new buffer that will fit an Eddystone URL frame
    //             with a compressed version of the given url; returns it in
    //             *frame and returns the size of the frame in bytes in *size,
    //             and frame is then owned by the caller, to be freed later with
    //             zjs_free
    //  returns: 0 for success, 1 for URL too long, 2 for out of memory, 3 for
    //             invalid url scheme/syntax (only http:// or https:// allowed)
    jerry_size_t sz = jerry_get_string_size(url);
    char buf[sz + 1];
    int len = jerry_string_to_char_buffer(url, (jerry_char_t *)buf, sz);
    buf[len] = '\0';

    // make sure it starts with http
    int offset = 0;
    if (strncmp(buf, "http", 4))
        return ZJS_URL_SCHEME_ERROR;
    offset += 4;

    int scheme = 0;
    if (buf[offset] == 's') {
        scheme++;
        offset++;
    }

    // make sure scheme http/https is followed by ://
    if (strncmp(buf + offset, "://", 3))
        return ZJS_URL_SCHEME_ERROR;
    offset += 3;

    if (strncmp(buf + offset, "www.", 4)) {
        scheme += 2;
    }
    else {
        offset += 4;
    }

    // FIXME: skipping the compression of .com, .com/, .org, etc for now

    len -= offset;
    if (len > 17)  // max URL length specified by Eddystone spec
        return ZJS_URL_TOO_LONG;

    uint8_t *ptr = zjs_malloc(len + 5);
    if (!ptr)
        return ZJS_ALLOC_FAILED;

    ptr[0] = 0xaa;  // Eddystone UUID
    ptr[1] = 0xfe;  // Eddystone UUID
    ptr[2] = 0x10;  // Eddystone-URL frame type
    ptr[3] = 0x00;  // calibrated Tx power at 0m
    ptr[4] = scheme; // encoded URL scheme prefix
    strncpy(ptr + 5, buf + offset, len);

    *size = len + 5;
    *frame = ptr;
    return ZJS_SUCCESS;
}

static jerry_value_t zjs_ble_start_advertising(const jerry_value_t function_obj,
                                               const jerry_value_t this,
                                               const jerry_value_t argv[],
                                               const jerry_length_t argc)
{
    // arg 0 should be the device name to advertise, e.g. "Arduino101"
    // arg 1 should be an array of UUIDs (short, 4 hex chars)
    // arg 2 should be a short URL (typically registered with Google, I think)
    char name[80];

    if (argc < 2 ||
        !jerry_value_is_string(argv[0]) ||
        !jerry_value_is_object(argv[1]) ||
        (argc >= 3 && !jerry_value_is_string(argv[2]))) {
        return zjs_error("zjs_ble_adv_start: invalid arguments");
    }

    jerry_value_t array = argv[1];
    if (!jerry_value_is_array(array)) {
        return zjs_error("zjs_ble_adv_start: expected array");
    }

    jerry_size_t sz = jerry_get_string_size(argv[0]);
    int len_name = jerry_string_to_char_buffer(argv[0],
                                               (jerry_char_t *) name,
                                               sz);
    name[len_name] = '\0';

    struct bt_data sd[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, name, len_name),
    };

    /*
     * Set Advertisement data. Based on the Eddystone specification:
     * https://github.com/google/eddystone/blob/master/protocol-specification.md
     * https://github.com/google/eddystone/tree/master/eddystone-url
     */
    uint8_t *url_frame = NULL;
    int frame_size;
    if (argc >= 3) {
        if (zjs_encode_url_frame(argv[2], &url_frame, &frame_size)) {
            PRINT("zjs_ble_start_advertising: error encoding url frame, won't be advertised\n");

            // TODO: Make use of error values and turn them into exceptions
        }
    }

    uint32_t arraylen = jerry_get_array_length(array);
    int records = arraylen;
    if (url_frame)
        records += 2;

    if (records == 0) {
        return zjs_error("zjs_ble_adv_start: nothing to advertise");
    }

    const uint8_t url_adv[] = { 0xaa, 0xfe };

    struct bt_data ad[records];
    int index = 0;
    if (url_frame) {
        ad[0].type = BT_DATA_UUID16_ALL;
        ad[0].data_len = 2;
        ad[0].data = url_adv;

        ad[1].type = BT_DATA_SVC_DATA16;
        ad[1].data_len = frame_size;
        ad[1].data = url_frame;

        index = 2;
    }

    for (int i=0; i<arraylen; i++) {
        jerry_value_t uuid;
        uuid = jerry_get_property_by_index(array, i);
        if (!jerry_value_is_string(uuid)) {
            return zjs_error("zjs_ble_adv_start: invalid uuid argument type");
        }

        jerry_size_t size = jerry_get_string_size(uuid);
        if (size != 4) {
            return zjs_error("zjs_ble_adv_start: unexpected uuid string length");
        }

        char ubuf[4];
        uint8_t bytes[2];
        jerry_string_to_char_buffer(uuid, (jerry_char_t *)ubuf, 4);
        if (!zjs_hex_to_byte(ubuf + 2, &bytes[0]) ||
            !zjs_hex_to_byte(ubuf, &bytes[1])) {
            return zjs_error("zjs_ble_adv_start: invalid character in uuid string");
        }

        ad[index].type = BT_DATA_UUID16_ALL;
        ad[index].data_len = 2;
        ad[index].data = bytes;
        index++;
    }

    int err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad),
                              sd, ARRAY_SIZE(sd));
    DBG_PRINT("=== Advertising Started ===\n");
    zjs_ble_queue_dispatch("advertisingStart", zjs_ble_adv_start_call_function, err);

    zjs_free(url_frame);
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_ble_stop_advertising(const jerry_value_t function_obj,
                                              const jerry_value_t this,
                                              const jerry_value_t argv[],
                                              const jerry_length_t argc)
{
    DBG_PRINT("zjs_ble_stop_advertising: stopAdvertising has been called\n");
    return ZJS_UNDEFINED;
}

static bool zjs_ble_parse_characteristic(struct zjs_ble_characteristic *chrc)
{
    char uuid[ZJS_BLE_UUID_LEN];
    if (!chrc || !chrc->chrc_obj)
        return false;

    jerry_value_t chrc_obj = chrc->chrc_obj;
    if (!zjs_obj_get_string(chrc_obj, "uuid", uuid, ZJS_BLE_UUID_LEN)) {
        PRINT("zjs_ble_parse_characteristic: characteristic uuid doesn't exist\n");
        return false;
    }

    chrc->uuid = zjs_ble_new_uuid_16(strtoul(uuid, NULL, 16));

    jerry_value_t v_array = zjs_get_property(chrc_obj, "properties");
    if (!jerry_value_is_array(v_array)) {
        PRINT("zjs_ble_parse_characteristic: properties is empty or not array\n");
        return false;
    }

    for (int i=0; i<jerry_get_array_length(v_array); i++) {
        jerry_value_t v_property = jerry_get_property_by_index(v_array, i);

        if (!jerry_value_is_string(v_property)) {
            PRINT("zjs_ble_parse_characteristic: property is not string\n");
            return false;
        }

        char name[20];
        jerry_size_t sz;
        sz = jerry_get_string_size(v_property);
        int len = jerry_string_to_char_buffer(v_property,
                                              (jerry_char_t *) name,
                                              sz);
        name[len] = '\0';

        if (!strcmp(name, "read")) {
            chrc->flags |= BT_GATT_CHRC_READ;
        } else if (!strcmp(name, "write")) {
            chrc->flags |= BT_GATT_CHRC_WRITE;
        } else if (!strcmp(name, "notify")) {
            chrc->flags |= BT_GATT_CHRC_NOTIFY;
        }
    }
    jerry_release_value(v_array);

    v_array = zjs_get_property(chrc_obj, "descriptors");
    if (!jerry_value_is_undefined(v_array) &&
        !jerry_value_is_null(v_array) &&
        !jerry_value_is_array(v_array)) {
        PRINT("zjs_ble_parse_characteristic: descriptors is not array\n");
        return false;
    }

    for (int i=0; i<jerry_get_array_length(v_array); i++) {
        jerry_value_t v_desc = jerry_get_property_by_index(v_array, i);

        if (!jerry_value_is_object(v_desc)) {
            PRINT("zjs_ble_parse_characteristic: not valid descriptor object\n");
            return false;
        }

        char desc_uuid[ZJS_BLE_UUID_LEN];
        if (!zjs_obj_get_string(v_desc, "uuid", desc_uuid, ZJS_BLE_UUID_LEN)) {
            PRINT("zjs_ble_parse_service: descriptor uuid doesn't exist\n");
            return false;
        }

        if (strtoul(desc_uuid, NULL, 16) == BT_UUID_GATT_CUD_VAL) {
            // Support CUD only, ignore all other type of descriptors
            jerry_value_t v_value = zjs_get_property(v_desc, "value");
            if (jerry_value_is_string(v_value)) {
                chrc->cud_value = jerry_acquire_value(v_value);
            }
        }
    }
    jerry_release_value(v_array);

    jerry_value_t v_func;
    v_func = zjs_get_property(chrc_obj, "onReadRequest");
    if (jerry_value_is_function(v_func)) {
        chrc->read_cb.zjs_cb.js_callback = jerry_acquire_value(v_func);
    }

    v_func = zjs_get_property(chrc_obj, "onWriteRequest");
    if (jerry_value_is_function(v_func)) {
        chrc->write_cb.zjs_cb.js_callback = jerry_acquire_value(v_func);
    }

    v_func = zjs_get_property(chrc_obj, "onSubscribe");
    if (jerry_value_is_function(v_func)) {
        chrc->subscribe_cb.zjs_cb.js_callback = jerry_acquire_value(v_func);
        // TODO: we need to monitor onSubscribe events from BLE driver eventually
        zjs_ble_subscribe_call_function(&chrc->subscribe_cb.zjs_cb);
    }

    v_func = zjs_get_property(chrc_obj, "onUnsubscribe");
    if (jerry_value_is_function(v_func)) {
        chrc->unsubscribe_cb.zjs_cb.js_callback = jerry_acquire_value(v_func);
    }

    v_func = zjs_get_property(chrc_obj, "onNotify");
    if (jerry_value_is_function(v_func)) {
        chrc->notify_cb.zjs_cb.js_callback = jerry_acquire_value(v_func);
    }

    return true;
}

static bool zjs_ble_parse_service(struct zjs_ble_service *service)
{
    char uuid[ZJS_BLE_UUID_LEN];
    if (!service || !service->service_obj)
        return false;

    jerry_value_t service_obj = service->service_obj;
    if (!zjs_obj_get_string(service_obj, "uuid", uuid, ZJS_BLE_UUID_LEN)) {
        PRINT("zjs_ble_parse_service: service uuid doesn't exist\n");
        return false;
    }
    service->uuid = zjs_ble_new_uuid_16(strtoul(uuid, NULL, 16));

    jerry_value_t v_array = zjs_get_property(service_obj, "characteristics");
    if (!jerry_value_is_array(v_array)) {
        PRINT("zjs_ble_parse_service: characteristics is empty or not array\n");
        return false;
    }

    struct zjs_ble_characteristic *previous = NULL;
    for (int i=0; i<jerry_get_array_length(v_array); i++) {
        jerry_value_t v_chrc = jerry_get_property_by_index(v_array, i);

        if (!jerry_value_is_object(v_chrc)) {
            PRINT("zjs_ble_parse_characteristic: characteristic is not object\n");
            return false;
        }

        struct zjs_ble_characteristic *chrc = zjs_malloc(sizeof(struct zjs_ble_characteristic));
        if (!chrc) {
            PRINT("zjs_ble_parse_service: out of memory allocating struct zjs_ble_characteristic\n");
            return false;
        }

        memset(chrc, 0, sizeof(struct zjs_ble_characteristic));

        chrc->chrc_obj = jerry_acquire_value(v_chrc);
        jerry_set_object_native_handle(chrc->chrc_obj, (uintptr_t)chrc, NULL);

        if (!zjs_ble_parse_characteristic(chrc)) {
            PRINT("failed to parse temp characteristic\n");
            return false;
        }

        // append to the list
        if (!service->characteristics) {
            service->characteristics = chrc;
            previous = chrc;
        }
        else {
           previous->next = chrc;
        }
    }

    return true;
}

static bool zjs_ble_register_service(struct zjs_ble_service *service)
{
    if (!service) {
        PRINT("zjs_ble_register_service: invalid ble_service\n");
        return false;
    }

    // calculate the number of GATT attributes to allocate
    int entry_index = 0;
    int num_of_entries = 1;   // 1 attribute for service uuid
    struct zjs_ble_characteristic *ch = service->characteristics;

    while (ch) {
        num_of_entries += 2;  // 2 attributes for uuid and descriptor

        if (ch->cud_value) {
            num_of_entries++; // 1 attribute for cud
        }

        if ((ch->flags & BT_GATT_CHRC_NOTIFY) == BT_GATT_CHRC_NOTIFY) {
            num_of_entries++; // 1 attribute for ccc
        }

        ch = ch->next;
    }

   struct bt_gatt_attr* bt_attrs = zjs_malloc(sizeof(struct bt_gatt_attr) * num_of_entries);
    if (!bt_attrs) {
        PRINT("zjs_ble_register_service: out of memory allocating struct bt_gatt_attr\n");
        return false;
    }

    // populate the array
    memset(bt_attrs, 0, sizeof(struct bt_gatt_attr) * num_of_entries);

    // GATT Primary Service
    bt_attrs[entry_index].uuid = zjs_ble_new_uuid_16(BT_UUID_GATT_PRIMARY_VAL);
    bt_attrs[entry_index].perm = BT_GATT_PERM_READ;
    bt_attrs[entry_index].read = bt_gatt_attr_read_service;
    bt_attrs[entry_index].user_data = service->uuid;
    entry_index++;

    ch = service->characteristics;
    while (ch) {
        // GATT Characteristic
        struct bt_gatt_chrc *chrc_user_data = zjs_malloc(sizeof(struct bt_gatt_chrc));
        if (!chrc_user_data) {
            PRINT("zjs_ble_register_service: out of memory allocating struct bt_gatt_chrc\n");
            return false;
        }

        memset(chrc_user_data, 0, sizeof(struct bt_gatt_chrc));

        chrc_user_data->uuid = ch->uuid;
        chrc_user_data->properties = ch->flags;
        bt_attrs[entry_index].uuid = zjs_ble_new_uuid_16(BT_UUID_GATT_CHRC_VAL);
        bt_attrs[entry_index].perm = BT_GATT_PERM_READ;
        bt_attrs[entry_index].read = bt_gatt_attr_read_chrc;
        bt_attrs[entry_index].user_data = chrc_user_data;

        // TODO: handle multiple descriptors
        // DESCRIPTOR
        entry_index++;
        bt_attrs[entry_index].uuid = ch->uuid;
        if (ch->read_cb.zjs_cb.js_callback) {
            bt_attrs[entry_index].perm |= BT_GATT_PERM_READ;
        }
        if (ch->write_cb.zjs_cb.js_callback) {
            bt_attrs[entry_index].perm |= BT_GATT_PERM_WRITE;
        }
        bt_attrs[entry_index].read = zjs_ble_read_attr_callback;
        bt_attrs[entry_index].write = zjs_ble_write_attr_callback;
        bt_attrs[entry_index].user_data = ch;

        // hold references to the GATT attr for sending notification
        ch->chrc_attr = &bt_attrs[entry_index];
        entry_index++;

        // CUD
        if (ch->cud_value) {
            jerry_size_t sz = jerry_get_string_size(ch->cud_value);
            char *cud_buffer = zjs_malloc(sz+1);
            if (!cud_buffer) {
                PRINT("zjs_ble_register_service: out of memory allocating cud buffer\n");
                return false;
            }

            memset(cud_buffer, 0, sz+1);

            jerry_string_to_char_buffer(ch->cud_value, (jerry_char_t *)cud_buffer, sz);
            bt_attrs[entry_index].uuid = zjs_ble_new_uuid_16(BT_UUID_GATT_CUD_VAL);
            bt_attrs[entry_index].perm = BT_GATT_PERM_READ;
            bt_attrs[entry_index].read = bt_gatt_attr_read_cud;
            bt_attrs[entry_index].user_data = cud_buffer;
            entry_index++;
        }

        // CCC
        if ((ch->flags & BT_GATT_CHRC_NOTIFY) == BT_GATT_CHRC_NOTIFY) {
            // add CCC only if notify flag is set
            struct _bt_gatt_ccc *ccc_user_data = zjs_malloc(sizeof(struct _bt_gatt_ccc));
            if (!ccc_user_data) {
                PRINT("zjs_ble_register_service: out of memory allocating struct bt_gatt_ccc\n");
                return false;
            }

            memset(ccc_user_data, 0, sizeof(struct _bt_gatt_ccc));

            ccc_user_data->cfg = zjs_ble_blvl_ccc_cfg;
            ccc_user_data->cfg_len = ARRAY_SIZE(zjs_ble_blvl_ccc_cfg);
            ccc_user_data->cfg_changed = zjs_ble_blvl_ccc_cfg_changed;
            bt_attrs[entry_index].uuid = zjs_ble_new_uuid_16(BT_UUID_GATT_CCC_VAL);
            bt_attrs[entry_index].perm = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE;
            bt_attrs[entry_index].read = bt_gatt_attr_read_ccc;
            bt_attrs[entry_index].write = bt_gatt_attr_write_ccc;
            bt_attrs[entry_index].user_data = ccc_user_data;
            entry_index++;
        }

        ch = ch->next;
    }

    if (entry_index != num_of_entries) {
        PRINT("zjs_ble_register_service: number of entries didn't match\n");
        return false;
    }

    DBG_PRINT("Registered service: %d entries\n", entry_index);
    bt_gatt_register(bt_attrs, entry_index);
    return true;
}

static jerry_value_t zjs_ble_set_services(const jerry_value_t function_obj,
                                          const jerry_value_t this,
                                          const jerry_value_t argv[],
                                          const jerry_length_t argc)
{
    // arg 0 should be an array of services
    // arg 1 is optionally an callback function
    if (argc < 1 ||
        !jerry_value_is_array(argv[0]) ||
        (argc > 1 && !jerry_value_is_function(argv[1]))) {
        return zjs_error("zjs_ble_set_services: invalid arguments");
    }

    // FIXME: currently hard-coded to work with demo
    // which has only 1 primary service and 2 characteristics
    // add support for multiple services
    jerry_value_t v_services = argv[0];
    int array_size = jerry_get_array_length(v_services);
    if (array_size == 0) {
        return zjs_error("zjs_ble_set_services: services array is empty");
    }

    // free existing services
    if (zjs_ble_services) {
        zjs_ble_free_services(zjs_ble_services);
        zjs_ble_services = NULL;
    }

    bool success = true;
    struct zjs_ble_service *previous = NULL;
    for (int i = 0; i < array_size; i++) {
        jerry_value_t v_service = jerry_get_property_by_index(v_services, i);

        if (!jerry_value_is_object(v_service)) {
            return zjs_error("zjs_ble_set_services: service is not object");
        }

        struct zjs_ble_service *service = zjs_malloc(sizeof(struct zjs_ble_service));
        if (!service) {
            return zjs_error("zjs_ble_set_services: out of memory allocating struct zjs_ble_service");
        }

        memset(service, 0, sizeof(struct zjs_ble_service));

        service->service_obj = jerry_acquire_value(v_service);
        jerry_set_object_native_handle(service->service_obj,
                                       (uintptr_t)service, NULL);

        if (!zjs_ble_parse_service(service)) {
            return zjs_error("zjs_ble_set_services: failed to parse service");
        }

        if (!zjs_ble_register_service(service)) {
            success = false;
            break;
        }

        // append to the list
        if (!zjs_ble_services) {
            zjs_ble_services = service;
            previous = service;
        }
        else {
           previous->next = service;
        }
    }

    if (argc > 1) {
        jerry_value_t arg;
        arg = success ? ZJS_UNDEFINED :
              jerry_create_string((jerry_char_t *)"failed to register services");
        jerry_value_t rval = jerry_call_function(argv[1], ZJS_UNDEFINED, &arg, 1);
        if (jerry_value_has_error_flag(rval)) {
            PRINT("zjs_ble_set_services: failed to call callback function\n");
        }
    }

    return ZJS_UNDEFINED;
}

// Constructor
static jerry_value_t zjs_ble_primary_service(const jerry_value_t function_obj,
                                             const jerry_value_t this,
                                             const jerry_value_t argv[],
                                             const jerry_length_t argc)
{
    if (argc < 1 || !jerry_value_is_object(argv[0])) {
        return zjs_error("zjs_ble_primary_service: invalid arguments");
    }

    return jerry_acquire_value(argv[0]);
}

// Constructor
static jerry_value_t zjs_ble_characteristic(const jerry_value_t function_obj,
                                            const jerry_value_t this,
                                            const jerry_value_t argv[],
                                            const jerry_length_t argc)
{
    if (argc < 1 || !jerry_value_is_object(argv[0])) {
        return zjs_error("zjs_ble_characterstic: invalid arguments");
    }

    jerry_value_t obj = jerry_acquire_value(argv[0]);
    jerry_value_t val;

    // error codes
    val = jerry_create_number(ZJS_BLE_RESULT_SUCCESS);
    zjs_set_property(obj, "RESULT_SUCCESS", val);
    jerry_release_value(val);

    val = jerry_create_number(ZJS_BLE_RESULT_INVALID_OFFSET);
    zjs_set_property(obj, "RESULT_INVALID_OFFSET", val);
    jerry_release_value(val);

    val = jerry_create_number(ZJS_BLE_RESULT_ATTR_NOT_LONG);
    zjs_set_property(obj, "RESULT_ATTR_NOT_LONG", val);
    jerry_release_value(val);

    val = jerry_create_number(ZJS_BLE_RESULT_INVALID_ATTRIBUTE_LENGTH);
    zjs_set_property(obj, "RESULT_INVALID_ATTRIBUTE_LENGTH", val);
    jerry_release_value(val);

    val = jerry_create_number(ZJS_BLE_RESULT_UNLIKELY_ERROR);
    zjs_set_property(obj, "RESULT_UNLIKELY_ERROR", val);
    jerry_release_value(val);

    return argv[0];
}

// Constructor
static jerry_value_t zjs_ble_descriptor(const jerry_value_t function_obj,
                                        const jerry_value_t this,
                                        const jerry_value_t argv[],
                                        const jerry_length_t argc)
{
    if (argc < 1 || !jerry_value_is_object(argv[0])) {
        return zjs_error("zjs_ble_descriptor: invalid arguments");
    }

    return jerry_acquire_value(argv[0]);
}

jerry_value_t zjs_ble_init()
{
     nano_sem_init(&zjs_ble_nano_sem);

    // create global BLE object
    jerry_value_t ble_obj = jerry_create_object();
    zjs_obj_add_function(ble_obj, zjs_ble_disconnect, "disconnect");
    zjs_obj_add_function(ble_obj, zjs_ble_on, "on");
    zjs_obj_add_function(ble_obj, zjs_ble_start_advertising, "startAdvertising");
    zjs_obj_add_function(ble_obj, zjs_ble_stop_advertising, "stopAdvertising");
    zjs_obj_add_function(ble_obj, zjs_ble_set_services, "setServices");

    // register constructors
    zjs_obj_add_function(ble_obj, zjs_ble_primary_service, "PrimaryService");
    zjs_obj_add_function(ble_obj, zjs_ble_characteristic, "Characteristic");
    zjs_obj_add_function(ble_obj, zjs_ble_descriptor, "Descriptor");
    return ble_obj;
}
#endif  // QEMU_BUILD
#endif  // BUILD_MODULE_BLE
