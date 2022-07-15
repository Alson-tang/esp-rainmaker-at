/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019-2025 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS ESP32 only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
#include "esp_system.h"

#ifdef CONFIG_AT_RAINMAKER_COMMAND_SUPPORT

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_at_core.h"

#include "esp_rmaker_core.h"
#include "esp_rmaker_standard_types.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_devices.h"
#include "esp_rmaker_schedule.h"
#include "esp_rmaker_scenes.h"
#include "esp_rmaker_console.h"

#define CHECK_PARAMS_NUM(cnt, total)                \
    if (cnt != total) {                             \
        return ESP_AT_RESULT_CODE_ERROR;            \
    }

#define RAINMAKER_PRE_CFG_PARTITION                 ("rmaker_pre")      // please refer to at_customize.csv
#define RAINMAKER_PRE_NAMESPACE                     ("pre_cfg_parms")

#define RAINMAKER_DYN_CFG_PARTITION                 ("rmaker_dyn")      // please refer to at_customize.csv
#define RAINMAKER_DYN_NAMESPACE                     ("dyn_cfg_parms")

#define RAINMAKER_PARAMS_MAX_SETS                   (7)
#define RAINMAKER_NODE_ATTR_MAX_SETS                (8)
#define RAINMAKER_PASSTHROUGH_BUFFER_LEN            (2048)

#define RAINMAKER_MESS_CONNECTED                    ("+RMCONNECTED:")
#define RAINMAKER_MESS_DISCONNECTED                 ("+RMDISCONNECTED:")
#define RAINMAKER_MESS_RMRESET                      ("+RMRESET:")
#define RAINMAKER_MESS_RMREBOOT                     ("+RMREBOOT:")
#define RAINMAKER_MESS_RMTIMEZONE                   ("+RMTIMEZONE:")

typedef enum {
    AT_START_RM_PROV = 0,
    AT_STOP_RM_PROV,
} at_rainmaker_prov_mode_t;

typedef enum {
    AT_RM_NORMAL = 0,
    AT_RM_PASSTHROUGH,
} at_rainmaker_mode_t;

typedef enum {
    AT_RM_CONNECTED = 0,
    AT_RM_DISCONNECTED,
    AT_RM_RESET,
    AT_RM_REBOOT,
    AT_RM_TIMEZONE,
} at_rainmaker_mess_t;

static const char *TAG = "rainmaker";

static SemaphoreHandle_t s_at_rainmaker_sync_sema;

static void at_rainmaker_wait_data_cb(void)
{
    xSemaphoreGive(s_at_rainmaker_sync_sema);
}

static void rmaker_response_result(at_rainmaker_mess_t message)
{
#define TEMP_BUFFER_SIZE    32
    uint8_t buffer[TEMP_BUFFER_SIZE] = {0};

    switch(message) {
        case AT_RM_CONNECTED:
            snprintf((char *)buffer, TEMP_BUFFER_SIZE, "\r\n%s", RAINMAKER_MESS_CONNECTED);
            break;

        case AT_RM_DISCONNECTED:
            snprintf((char *)buffer, TEMP_BUFFER_SIZE, "\r\n%s", RAINMAKER_MESS_DISCONNECTED);
            break;

        case AT_RM_RESET:
            snprintf((char *)buffer, TEMP_BUFFER_SIZE, "\r\n%s", RAINMAKER_MESS_RMRESET);
            break;

        case AT_RM_REBOOT:
            snprintf((char *)buffer, TEMP_BUFFER_SIZE, "\r\n%s", RAINMAKER_MESS_RMREBOOT);
            break;

        case AT_RM_TIMEZONE:
            snprintf((char *)buffer, TEMP_BUFFER_SIZE, "\r\n%s", RAINMAKER_MESS_RMTIMEZONE);
            break;

        default:
            break;

    }

    esp_at_port_write_data(buffer, strlen((char *)buffer));

    return;
}

static uint8_t at_exe_cmd_rmnodeinit(uint8_t *cmd_name)
{
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmnodeattradd(uint8_t para_num)
{
    int cnt = 0;
    uint8_t *name_1 = NULL;
    uint8_t *value_1 = NULL;
    uint8_t *name = NULL;
    uint8_t *value = NULL;

    // name_1
    if (esp_at_get_para_as_str(cnt++, &name_1) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // value_1
    if (esp_at_get_para_as_str(cnt++, &value_1) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if (cnt != para_num) {
        for (int i = 0; i < (RAINMAKER_NODE_ATTR_MAX_SETS - 1); i++) {
            if (cnt != para_num) {
                // next set of attribute's name
                if (esp_at_get_para_as_str(cnt++, &name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
                    return ESP_AT_RESULT_CODE_ERROR;
                }

                if (cnt != para_num) {
                    // next set of attribute's value
                    if (esp_at_get_para_as_str(cnt++, &value) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
                        return ESP_AT_RESULT_CODE_ERROR;
                    }
                }
            }
        }
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmdevadd(uint8_t para_num)
{
    int32_t cnt = 0;
    uint8_t *unique_name = NULL;
    uint8_t *device_name = NULL;
    uint8_t *device_type = NULL;

    // unique_name
    if (esp_at_get_para_as_str(cnt++, &unique_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // device_name
    if (esp_at_get_para_as_str(cnt++, &device_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // device_type
    if (esp_at_get_para_as_str(cnt++, &device_type) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmdevdel(uint8_t para_num)
{
    int32_t cnt = 0;
    uint8_t *unique_name = NULL;

    // unique_name
    if (esp_at_get_para_as_str(cnt++, &unique_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmparamadd(uint8_t para_num)
{
    int32_t cnt = 0;
    uint8_t *unique_name = NULL;
    uint8_t *param_name = NULL;
    uint8_t *param_type = NULL;
    uint8_t *data_type = NULL;
    int32_t properties = 0;
    uint8_t *ui_type = NULL;
    int32_t min = 0;
    int32_t max = 0;
    int32_t step = 0;
    int32_t def = 0;

    // unique_name
    if (esp_at_get_para_as_str(cnt++, &unique_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // param_name
    if (esp_at_get_para_as_str(cnt++, &param_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // param_type
    if (esp_at_get_para_as_str(cnt++, &param_type) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // data_type
    if (esp_at_get_para_as_str(cnt++, &data_type) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // properties
    if (esp_at_get_para_as_digit(cnt++, &properties) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // ui_type
    if (esp_at_get_para_as_str(cnt++, &ui_type) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // min
    if (esp_at_get_para_as_digit(cnt++, &min) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // max
    if (esp_at_get_para_as_digit(cnt++, &max) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // step
    if (esp_at_get_para_as_digit(cnt++, &step) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // def
    if (esp_at_get_para_as_digit(cnt++, &def) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmparamdel(uint8_t para_num)
{
    int cnt = 0;
    uint8_t *unique_name = NULL;

    // unique_name
    if (esp_at_get_para_as_str(cnt++, &unique_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmusermapping(uint8_t para_num)
{
    int cnt = 0;
    uint8_t *user_id = NULL;
    uint8_t *key = NULL;

    // user_id
    if (esp_at_get_para_as_str(cnt++, &user_id) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // key
    if (esp_at_get_para_as_str(cnt++, &key) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_exe_cmd_rmuserunmapping(uint8_t *cmd_name)
{
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmprov(uint8_t para_num)
{
    int32_t cnt = 0;
    int32_t mode = AT_START_RM_PROV;
    uint8_t *customer_id = NULL;
    uint8_t *device_extra_code = NULL;
    uint8_t *broadcase_name = NULL;

    // mode
    if (esp_at_get_para_as_digit(cnt++, &mode) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if ((mode != AT_START_RM_PROV) && (mode != AT_STOP_RM_PROV)) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // customer_id
    if (cnt != para_num) {
        if (esp_at_get_para_as_str(cnt++, &customer_id) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
            return ESP_AT_RESULT_CODE_ERROR;
        }
    }

    // device_extra code
    if (cnt != para_num) {
        if (esp_at_get_para_as_str(cnt++, &device_extra_code) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
            return ESP_AT_RESULT_CODE_ERROR;
        }
    }

    // broadcase_name
    if (cnt != para_num) {
        if (esp_at_get_para_as_str(cnt++, &broadcase_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
            return ESP_AT_RESULT_CODE_ERROR;
        }
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmconn(uint8_t *cmd_name)
{
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmclose(uint8_t *cmd_name)
{
    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmparamupdate(uint8_t para_num)
{
    int cnt = 0;
    uint8_t *unique_name = NULL;
    uint8_t *param_name_1 = NULL;
    uint8_t *param_value_1 = NULL;
    uint8_t *param_name = NULL;
    uint8_t *param_value = NULL;

    // unique_name
    if (esp_at_get_para_as_str(cnt++, &unique_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // param_name_1
    if (esp_at_get_para_as_str(cnt++, &param_name_1) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // param_value_1
    if (esp_at_get_para_as_str(cnt++, &param_value_1) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if (cnt != para_num) {
        for (int i = 0; i < (RAINMAKER_PARAMS_MAX_SETS - 1); i++) {
            if (cnt != para_num) {
                // next set of parameter name
                if (esp_at_get_para_as_str(cnt++, &param_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
                    return ESP_AT_RESULT_CODE_ERROR;
                }

                if (cnt != para_num) {
                    // next set of parameter value
                    if (esp_at_get_para_as_str(cnt++, &param_value) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
                        return ESP_AT_RESULT_CODE_ERROR;
                    }
                }
            }
        }
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmmode(uint8_t para_num)
{
    int cnt = 0;
    int32_t mode = AT_RM_NORMAL;
    uint8_t *buf = NULL;
    int32_t had_received_len = 0;

    // mode
    if (esp_at_get_para_as_digit(cnt++, &mode) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if ((mode != AT_RM_NORMAL) && (mode != AT_RM_PASSTHROUGH)) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    if (mode == AT_RM_PASSTHROUGH) {
        // data processing
        buf = (uint8_t *)calloc(1, RAINMAKER_PASSTHROUGH_BUFFER_LEN);
        if (buf == NULL) {
            return ESP_AT_RESULT_CODE_ERROR;
        }

        if (!s_at_rainmaker_sync_sema) {
            s_at_rainmaker_sync_sema = xSemaphoreCreateBinary();
            if (!s_at_rainmaker_sync_sema) {
                free(buf);
                buf = NULL;
                return ESP_AT_RESULT_CODE_ERROR;
            }
        }

        esp_at_port_enter_specific(at_rainmaker_wait_data_cb);

        // output "OK" and ">"
        esp_at_response_result(ESP_AT_RESULT_CODE_OK_AND_INPUT_PROMPT);

        // receive at cmd port data
        while (xSemaphoreTake(s_at_rainmaker_sync_sema, portMAX_DELAY)) {
            memset(buf, 0, RAINMAKER_PASSTHROUGH_BUFFER_LEN);

            had_received_len = esp_at_port_read_data(buf, RAINMAKER_PASSTHROUGH_BUFFER_LEN);

            // Check whether to exit the passthrough mode
            // the exit condition is the "+++" string received
            if ((had_received_len == 3) && (strncmp((const char *)buf, "+++", 3) == 0)) {
                esp_at_port_exit_specific();

                break;
            } else {
                // process the received data
                printf("%s\r\n", buf);
            }
        }

        vSemaphoreDelete(s_at_rainmaker_sync_sema);
        s_at_rainmaker_sync_sema = NULL;

        free(buf);
        buf = NULL;

        return ESP_AT_RESULT_CODE_PROCESS_DONE;
    }

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_rmsend(uint8_t para_num)
{
#define TEMP_BUFFER_SIZE    32
    uint8_t buffer[TEMP_BUFFER_SIZE] = {0};
    int cnt = 0;
    uint8_t *unique_name = NULL;
    uint8_t *param_name = NULL;
    int32_t len = 0;
    uint8_t *buf = NULL;
    int32_t had_received_len = 0;

    // unique_name
    if (esp_at_get_para_as_str(cnt++, &unique_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // param_name
    if (esp_at_get_para_as_str(cnt++, &param_name) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // len
    if (esp_at_get_para_as_digit(cnt++, &len) == ESP_AT_PARA_PARSE_RESULT_FAIL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    // parameters are ready
    CHECK_PARAMS_NUM(cnt, para_num);

    // data processing
    buf = (uint8_t *)calloc(1, len);
    if (buf == NULL) {
        return ESP_AT_RESULT_CODE_ERROR;
    }

    if (!s_at_rainmaker_sync_sema) {
        s_at_rainmaker_sync_sema = xSemaphoreCreateBinary();
        if (!s_at_rainmaker_sync_sema) {
            free(buf);
            buf = NULL;
            return ESP_AT_RESULT_CODE_ERROR;
        }
    }

    esp_at_port_enter_specific(at_rainmaker_wait_data_cb);

    // output "OK" and ">"
    esp_at_response_result(ESP_AT_RESULT_CODE_OK_AND_INPUT_PROMPT);

    // receive at cmd port data
    while (xSemaphoreTake(s_at_rainmaker_sync_sema, portMAX_DELAY)) {
        had_received_len += esp_at_port_read_data(buf + had_received_len, len - had_received_len);
        if (had_received_len == len) {
            esp_at_port_exit_specific();

            snprintf((char *)buffer, TEMP_BUFFER_SIZE, "\r\nRecv %d bytes\r\n", len);
            esp_at_port_write_data(buffer, strlen((char *)buffer));

            had_received_len = esp_at_port_get_data_length();
            if (had_received_len > 0) {
                snprintf((char *)buffer, TEMP_BUFFER_SIZE, "\r\nbusy p...\r\n");
                esp_at_port_write_data(buffer, strlen((char *)buffer));
            }

            break;
        }
    }

    vSemaphoreDelete(s_at_rainmaker_sync_sema);
    s_at_rainmaker_sync_sema = NULL;

    free(buf);
    buf = NULL;

    return ESP_AT_RESULT_CODE_SEND_OK;
}

static esp_err_t rmaker_get_params(void)
{
    esp_err_t ret = ESP_OK;
    const esp_partition_t *partition = NULL;
    nvs_handle_t rmaker_handle;
    size_t len = 0;
    uint8_t *buf = NULL;

    partition = esp_at_custom_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, RAINMAKER_PRE_CFG_PARTITION);
    if (partition == NULL) {
        ESP_LOGE(TAG, "partition find failed");
        return ESP_FAIL;
    }

    ret = nvs_flash_init_partition_ptr(partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "partition init failed, ret: 0x%04X", ret);
        return ESP_FAIL;
    }

    ret = nvs_open_from_partition(RAINMAKER_PRE_CFG_PARTITION, RAINMAKER_PRE_NAMESPACE, NVS_READWRITE, &rmaker_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "partition open failed, ret: 0x%04X", ret);
    }

    ret = nvs_get_str(rmaker_handle, "param_name", NULL, &len);
    switch (ret) {
        case ESP_OK: {
            buf = (uint8_t *)calloc(1, len + 1);
            ret = nvs_get_str(rmaker_handle, "param_name", (char *)buf, &len);
            if (ret != ESP_OK) {
                ESP_LOGD(TAG, "error (%s) reading!", esp_err_to_name(ret));
            }
            break;
        }
        case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGD(TAG, "the string is not initialized yet");
            break;
        default :
            ESP_LOGD(TAG, "error (%s) reading!", esp_err_to_name(ret));
    }

    // Close
    nvs_close(rmaker_handle);

    return ESP_OK;
}

static const esp_at_cmd_struct s_at_rainmaker_cmd[] = {
    {"+RMNODEINIT", NULL, NULL, NULL, at_exe_cmd_rmnodeinit},
    {"+RMNODEATTRADD", NULL, NULL, at_setup_cmd_rmnodeattradd, NULL},
    {"+RMDEVADD", NULL, NULL, at_setup_cmd_rmdevadd, NULL},
    {"+RMDEVDEL", NULL, NULL, at_setup_cmd_rmdevdel, NULL},
    {"+RMPARAMADD", NULL, NULL, at_setup_cmd_rmparamadd, NULL},
    {"+RMPARAMDEL", NULL, NULL, at_setup_cmd_rmparamdel, NULL},
    {"+RMUSERMAPPING", NULL, NULL, at_setup_cmd_rmusermapping, NULL},
    {"+RMUSERUNMAPPING", NULL, NULL, NULL, at_exe_cmd_rmuserunmapping},
    {"+RMPROV", NULL, NULL, at_setup_cmd_rmprov, NULL},
    {"+RMCONN", NULL, NULL, NULL, at_setup_cmd_rmconn},
    {"+RMCLOSE", NULL, NULL, NULL, at_setup_cmd_rmclose},
    {"+RMPARAMUPDATE", NULL, NULL, at_setup_cmd_rmparamupdate, NULL},
    {"+RMMODE", NULL, NULL, at_setup_cmd_rmmode, NULL},
    {"+RMSEND", NULL, NULL, at_setup_cmd_rmsend, NULL},
};

bool esp_at_rainmaker_cmd_regist(void)
{
    return esp_at_custom_cmd_array_regist(s_at_rainmaker_cmd, sizeof(s_at_rainmaker_cmd) / sizeof(s_at_rainmaker_cmd[0]));
}

#endif
