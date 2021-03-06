/*
 * Copyright 2015 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 * @brief Simple interface to load and run javascript from our code memory stash
 *
 * Reads a program from the ROM/RAM
 */

/* Zephyr includes */
#include <zephyr.h>
#include <malloc.h>
#include <string.h>
#include <ctype.h>

/* JerryScript includes */
#include "jerry-api.h"
#include "jerry-port.h"

#include "file-utils.h"

/* Zephyr.js init everything */
#include "../zjs_buffer.h"
#include "../zjs_callbacks.h"
#include "../zjs_modules.h"
#include "../zjs_ipm.h"
#include "../zjs_sensor.h"
#include "../zjs_timers.h"

void jerry_port_default_set_log_level(jerry_log_level_t level); /** Inside jerry-port-default.h */

#include "comms-uart.h"

static jerry_value_t parsed_code = 0;

#define MAX_BUFFER_SIZE 4096

static void javascript_print_value(const jerry_value_t value)
{
    if (jerry_value_is_undefined(value)) {
        jerry_port_console("undefined");
    } else if (jerry_value_is_null(value)) {
        jerry_port_console("null");
    } else if (jerry_value_is_boolean(value)) {
        if (jerry_get_boolean_value(value)) {
            jerry_port_console("true");
        } else {
            jerry_port_console("false");
        }
    }
    /* Float value */
    else if (jerry_value_is_number(value)) {
        double val = jerry_get_number_value(value);
        // %lf prints an empty value :?
        jerry_port_console("Number [%d]\n", (int) val);
    }
    /* String value */
    else if (jerry_value_is_string(value)) {
        /* Determining required buffer size */
        jerry_size_t req_sz = jerry_get_string_size(value);
        jerry_char_t str_buf_p[req_sz];

        jerry_string_to_char_buffer(value, str_buf_p, req_sz);
        str_buf_p[req_sz] = '\0';

        jerry_port_console("%s", (const char *)str_buf_p);
    }
    /* Object reference */
    else if (jerry_value_is_object(value)) {
        jerry_port_console("[JS object]");
    }

    jerry_port_console("\n");
}

static void javascript_print_error(jerry_value_t error_value)
{
    if (!jerry_value_has_error_flag(error_value))
        return;

    jerry_value_clear_error_flag(&error_value);
    jerry_value_t err_str_val = jerry_value_to_string(error_value);
    jerry_size_t err_str_size = jerry_get_string_size(err_str_val);
    jerry_char_t err_str_buf[256];

    if (err_str_size >= 256) {
        const char msg[] = "[Error message too long]";
        err_str_size = sizeof(msg) / sizeof(char) - 1;
        memcpy(err_str_buf, msg, err_str_size);
    } else {
        jerry_string_to_char_buffer(err_str_val, err_str_buf, err_str_size);
    }
    err_str_buf[err_str_size] = 0;

    jerry_port_log(JERRY_LOG_LEVEL_ERROR, err_str_buf);
    jerry_port_log(JERRY_LOG_LEVEL_ERROR, "\n");
    jerry_release_value(err_str_val);
}

void javascript_eval_code(const char *source_buffer)
{
    jerry_value_t ret_val;

    jerry_port_default_set_log_level(JERRY_LOG_LEVEL_TRACE);
    ret_val = jerry_eval((jerry_char_t *) source_buffer,
                         strnlen(source_buffer, MAX_BUFFER_SIZE),
                         false);

    if (jerry_value_has_error_flag(ret_val)) {
        printf("[ERR] failed to evaluate JS\n");
        javascript_print_error(ret_val);
    } else {
        if (!jerry_value_is_undefined(ret_val))
            javascript_print_value(ret_val);
    }

    jerry_release_value(ret_val);
}

void restore_zjs_api() {
#ifdef ZJS_POOL_CONFIG
    zjs_init_mem_pools();
#ifdef DUMP_MEM_STATS
    zjs_print_pools();
#endif
#endif
    jerry_init(JERRY_INIT_EMPTY);
    zjs_timers_init();
#ifdef BUILD_MODULE_CONSOLE
    zjs_console_init();
#endif
#ifdef BUILD_MODULE_BUFFER
    zjs_buffer_init();
#endif
#ifdef BUILD_MODULE_SENSOR
    zjs_sensor_init();
#endif
    zjs_init_callbacks();
    zjs_modules_init();
}

void javascript_stop()
{
    if (parsed_code == 0)
        return;

    /* Parsed source code must be freed */
    jerry_release_value(parsed_code);
    parsed_code = 0;

    /* Cleanup engine */
    zjs_timers_cleanup();
    zjs_ipm_free_callbacks();
#ifdef BUILD_MODULE_BUFFER
    zjs_buffer_cleanup();
#endif
#ifdef BUILD_MODULE_SENSOR
    zjs_sensor_cleanup();
#endif
    zjs_modules_cleanup();
    jerry_cleanup();

    restore_zjs_api();
}

int javascript_parse_code(const char *file_name, bool show_lines)
{
    int ret = -1;
    javascript_stop();
    jerry_port_default_set_log_level(JERRY_LOG_LEVEL_TRACE);
    char *buf = NULL;

    fs_file_t *fp = fs_open_alloc(file_name, "r");

    if (fp == NULL)
        return ret;

    ssize_t size = fs_size(fp);
    if (size == 0) {
        comms_printf("[ERR] Empty file (%s)\n", file_name);
        goto cleanup;
    }

    buf = (char *)malloc(size);
    if (buf == NULL) {
        comms_printf("[ERR] Not enough memory for (%s)\n", file_name);
        goto cleanup;
    }

    ssize_t brw = fs_read(fp, buf, size);

    if (brw != size) {
        comms_printf("[ERR] Failed loading code %s\n", file_name);
        goto cleanup;
    }

    if (show_lines) {
        comms_printf("[READ] %d\n", (int)brw);

        // Print buffer test
        int line = 0;
        comms_println("[START]");
        comms_printf("%5d  ", line++);
        for (int t = 0; t < brw; t++) {
            uint8_t byte = buf[t];
            if (byte == '\n' || byte == '\r') {
                comms_write_buf("\r\n", 2);
                comms_printf("%5d  ", line++);
            } else {
                if (!isprint(byte)) {
                    comms_printf("(%x)", byte);
                } else
                    comms_writec(byte);
            }
        }
        comms_println("[END]");
    }

    /* Setup Global scope code */
    parsed_code = jerry_parse((const jerry_char_t *) buf, size, false);
    if (jerry_value_has_error_flag(parsed_code)) {
        printf("[ERR] Could not parse JS\n");
        javascript_print_error(parsed_code);
        jerry_release_value(parsed_code);
    } else {
        ret = 0;
    }

cleanup:
    fs_close_alloc(fp);
    free(buf);
    return ret;
}

void javascript_run_code(const char *file_name)
{
    if (javascript_parse_code(file_name, false) != 0)
        return;

    /* Execute the parsed source code in the Global scope */
    jerry_value_t ret_value = jerry_run(parsed_code);

    if (jerry_value_has_error_flag(ret_value)) {
        javascript_print_error(ret_value);
    }

    /* Returned value must be freed */
    jerry_release_value(ret_value);
}

void javascript_run_snapshot(const char *file_name)
{

}
