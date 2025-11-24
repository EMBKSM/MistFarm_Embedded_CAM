/**
 * @file main.c
 * @brief ESP32-CAM을 사용하여 이미지를 캡처하고 AI 서버로 전송하는 펌웨어이다.
 * @details 이 파일은 WiFi 연결, 카메라 초기화, 그리고 HTTP POST(Multipart) 요청을 통한 이미지 전송 기능을 수행한다.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

/** WiFi SSID 설정이다. */
#define WIFI_SSID       "YOUR_WIFI_SSID"
/** WiFi 비밀번호 설정이다. */
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"

/** * @brief AI 분석 서버의 URL 주소이다.
 * @note Ngrok 또는 공인 IP 주소를 사용하며 /predict 엔드포인트를 포함한다.
 */
#define SERVER_URL      "https://workaday-luciano-unresentfully.ngrok-free.dev/predict"

/** 기기의 고유 ID 값이다. */
#define DEVICE_ID       "284910245"
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0      5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

static const char *TAG = "MISTFARM_CAM";

/**
 * @brief WiFi 및 IP 이벤트를 처리하는 핸들러 함수이다.
 * @param arg 사용자 인자이다.
 * @param event_base 이벤트 베이스이다.
 * @param event_id 이벤트 ID이다.
 * @param event_data 이벤트 데이터 포인터이다.
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi 연결 재시도");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi 연결 성공 IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

/**
 * @brief WiFi 스테이션 모드를 초기화하고 연결을 시작한다.
 * @details NVS 초기화 및 이벤트 루프 생성을 포함하며 WiFi 설정을 로드한다.
 */
void wifi_init_sta(void) {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    ESP_LOGI(TAG, "WiFi 초기화 완료.");
}

/**
 * @brief 카메라 하드웨어를 초기화하고 설정을 적용한다.
 * @details AI-Thinker 모듈의 핀맵을 적용하며 PSRAM 유무에 따라 해상도를 조정한다.
 * @return 초기화 성공 시 ESP_OK, 실패 시 에러 코드를 반환한다.
 */
esp_err_t camera_init() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_pclk = CAM_PIN_PCLK;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        config.frame_size = FRAMESIZE_UXGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
        ESP_LOGI(TAG, "PSRAM이 감지됨. 고화질 모드로 설정");
    } else {
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        ESP_LOGW(TAG, "PSRAM이 감지. 저화질 모드로 설정");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "카메라 초기화 실패: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "카메라 초기화 성공");
    return ESP_OK;
}

/**
 * @brief 캡처된 이미지를 서버로 전송한다.
 * @details Multipart/form-data 형식을 사용하여 device_id와 이미지 데이터를 전송한다.
 * @return 전송 성공 시 ESP_OK, 실패 시 ESP_FAIL을 반환한다.
 */
esp_err_t send_image_to_server() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "사진 캡처 실패");
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = NULL,
        .skip_cert_common_name_check = true, 
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    const char *boundary = "MistFarmBoundary";
    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    char *body_head_fmt = "--%s\r\nContent-Disposition: form-data; name=\"device_id\"\r\n\r\n%s\r\n"
                          "--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"capture.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    
    int head_len = snprintf(NULL, 0, body_head_fmt, boundary, DEVICE_ID, boundary);
    char *body_head = malloc(head_len + 1);
    sprintf(body_head, body_head_fmt, boundary, DEVICE_ID, boundary);

    char *body_tail_fmt = "\r\n--%s--\r\n";
    int tail_len = snprintf(NULL, 0, body_tail_fmt, boundary);
    char *body_tail = malloc(tail_len + 1);
    sprintf(body_tail, body_tail_fmt, boundary);

    esp_http_client_open(client, head_len + fb->len + tail_len);
    
    esp_http_client_write(client, body_head, head_len);
    esp_http_client_write(client, (const char *)fb->buf, fb->len);
    esp_http_client_write(client, body_tail, tail_len);

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length >= 0) {
        char *response_buf = malloc(content_length + 1);
        int read_len = esp_http_client_read_response(client, response_buf, content_length);
        if (read_len >= 0) {
            response_buf[read_len] = 0;
            ESP_LOGI(TAG, "서버 응답을 수신: %s", response_buf);
        }
        free(response_buf);
    } else {
        ESP_LOGE(TAG, "서버 응답 없음");
    }

    free(body_head);
    free(body_tail);
    esp_http_client_cleanup(client);
    esp_camera_fb_return(fb);

    return ESP_OK;
}

/**
 * @brief 메인 애플리케이션 진입점이다.
 * @details WiFi 및 카메라를 초기화하고 주기적으로 이미지를 촬영하여 전송하는 루프를 실행한다.
 */
void app_main(void) {
    wifi_init_sta();

    if (camera_init() != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        ESP_LOGI(TAG, "사진 촬영 및 전송 시작");
        
        esp_err_t res = send_image_to_server();
        
        if (res == ESP_OK) {
            ESP_LOGI(TAG, "전송 완료. 10초간 대기.");
        } else {
            ESP_LOGE(TAG, "전송 실패. 10초 후 재시도.");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}