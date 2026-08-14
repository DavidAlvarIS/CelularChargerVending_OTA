#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h" 
#include "esp_ota_ops.h"
#include "esp_mac.h"  // Para obtener la dirección MAC    

#define positionlights GPIO_NUM_8

#define VERSION "1.0.0" // Versión actual del firmware
#define VERSION_URL "https://raw.githubusercontent.com/DavidAlvarIS/CelularChargerVending_OTA/refs/heads/main/version.txt?token=GHSAT0AAAAAAEFFI5TTW55WC3XPU3OFKTQU2T6COVQ"

// Dirección MAC esperada (cámbiala por la de tu ESP32)
#define EXPECTED_MAC {0x1C, 0xDB, 0xD4, 0x35, 0x96, 0x50}  // Reemplaza XX con los bytes reales

static const char *TAG = "CelChargeVending"; // Nombre de mi chip y etiqueta para logs. 
#define STACK_SIZE 1024 * 2

// Variables para progreso OTA
static size_t total_size = 0;
static size_t downloaded = 0;

// ---- CONFIGURACIÓN WIFI MODO STA ----

#define WIFI_SSID "Max3D_Tech"
#define WIFI_PASS "12345678"

static EventGroupHandle_t wifi_event_group; // Variable para manejar eventos de WiFi
const int CONNECTED_BIT = BIT0;             // Bit para indicar que el dispositivo está conectado a WiFi
static int retry_num = 0;
#define MAXIMUM_RETRY 10


int BlinkTime = 500; // Tiempo de parpadeo en milisegundos

// Prototipos de funciones
esp_err_t init_PIN(void);
esp_err_t create_tasks(void);
static void wifi_shutdown(void);

void vTask_ota_task(void *pvParameters);
void vtask_Basic_LED_Blink(void *pvParameters);

esp_err_t init_PIN(void)
{
    gpio_reset_pin(positionlights);
    gpio_set_direction(positionlights, GPIO_MODE_OUTPUT);

    return ESP_OK;
}

esp_err_t create_tasks(void)
{
    static uint8_t ucParameterToPass;
    TaskHandle_t xHandle = NULL;

    xTaskCreate(&vTask_ota_task,
                "vTask_ota_task",
                16 * 1024,
                &ucParameterToPass,
                5,
                &xHandle);

    xTaskCreate(vtask_Basic_LED_Blink,
                "vtask_Basic_LED_Blink",
                STACK_SIZE,
                &ucParameterToPass,
                10,
                &xHandle);

    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        // ESP_LOGE(TAG,"WiFi desconectado\n");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        retry_num++;
        if (retry_num < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            ESP_LOGI(TAG, "WiFi desconectado. Reintentando conexión... (Intento %d)", retry_num);
        }
        else
        {
            ESP_LOGI(TAG, "WiFi deshabilitado después de %d intentos fallidos.", MAXIMUM_RETRY);
            wifi_shutdown();
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi Conectado!. IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_num = 0;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA iniciado. Conectando a %s...", WIFI_SSID);
}

/**
 * @brief Apaga el WiFi y libera recursos asociados.
 *        Se llama cuando la tarea OTA finaliza (con o sin éxito) y ya no necesita conectividad.
 */
static void wifi_shutdown(void)
{
    ESP_LOGI(TAG, "Apagando WiFi...");
    // Desregistrar manejadores de eventos
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, NULL);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    // Detener y desinicializar WiFi
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_LOGI(TAG, "WiFi apagado.");
}


bool check_for_update(void)
{
    esp_http_client_config_t http_config = {
        .url = VERSION_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 128,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    esp_err_t err = esp_http_client_open(client, 0); // write_len=0 para GET
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error al abrir version.txt: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    // Leer los encabezados para obtener el código de estado
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Error al obtener encabezados HTTP");
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "Respuesta HTTP %d", status_code);
        esp_http_client_cleanup(client);
        return false;
    }

    char buffer[64] = {0};
    int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
    if (read_len <= 0) {
        ESP_LOGE("CelChargeVending", "Error al leer version.txt");
        esp_http_client_cleanup(client);
        return false;
    }
    buffer[read_len] = '\0';

    // Limpiar saltos de línea
    char *newline = strchr(buffer, '\r');
    if (newline) *newline = '\0';
    newline = strchr(buffer, '\n');
    if (newline) *newline = '\0';

    ESP_LOGI("CelChargeVending", "Versión actual: %s, Versión remota: %s", VERSION, buffer);
    bool update_needed = (strcmp(VERSION, buffer) != 0);
    esp_http_client_cleanup(client);
    return update_needed;
}

esp_err_t progress_callback(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (strcmp(evt->header_key, "Content-Length") == 0) {
                total_size = atoi(evt->header_value);
                ESP_LOGI("CelChargeVending", "Tamaño total: %u bytes", total_size);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            downloaded += evt->data_len;
            if (total_size > 0) {
                int progress = (downloaded * 100) / total_size;
                ESP_LOGI("CelChargeVending", "Progreso: %d%% (%u/%u bytes)", progress, downloaded, total_size);
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            total_size = 0;
            downloaded = 0;
            break;
        default:
            break;
    }
    return ESP_OK;
}

void vTask_ota_task(void *pvParameter)
{
    // Esperar conexión WiFi con timeout de 30 segundos
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(30000));

    if ((bits & CONNECTED_BIT) == 0)
    {
        ESP_LOGI("CelChargeVending", "No se pudo conectar a WiFi en 30s. OTA cancelada.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI("CelChargeVending", "WiFi conectado, comprobando versión...");
    ESP_LOGI("CelChargeVending", "Free heap: %lu", esp_get_free_heap_size());

    if (!check_for_update())// 
    {
        ESP_LOGI("CelChargeVending", "Versión actual ya es la última. No se requiere OTA.");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI("CelChargeVending", "Nueva versión disponible. Iniciando OTA...");
    esp_http_client_config_t config = {
        .url = "https://github.com/DavidAlvarIS/CelularChargerVending_OTA/releases/download/v1.0.0/CelularChargerVending_OTA.bin",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 16384,
        .buffer_size_tx = 1024,
        .timeout_ms = 15000,
        .keep_alive_enable = false,
        .event_handler = progress_callback,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK)
    {
        ESP_LOGI("CelChargeVending", "OTA completada exitosamente. Reiniciando...");
        esp_restart();
    }
    else
    {
        ESP_LOGE("CelChargeVending", "OTA falló. Código: 0x%x", ret);
        wifi_shutdown();   // <--- Apagar WiFi antes de salir
    }

    vTaskDelete(NULL);
}

void vtask_Basic_LED_Blink(void *pvParameters)
{

    while (1)
    {
        // Control de luces de posición
        gpio_set_level(positionlights, 1); // Encender luces de posición
        ESP_LOGI(TAG, "Luces de posición encendidas");
        vTaskDelay(pdMS_TO_TICKS(BlinkTime));    // Esperar 1 segundo
        gpio_set_level(positionlights, 0); // Apagar luces de posición
        ESP_LOGI(TAG, "Luces de posición apagadas");
        vTaskDelay(pdMS_TO_TICKS(BlinkTime));    // Esperar 1 segundo
    }
}

// Función para verificar la dirección MAC

bool print_mac_address() {
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA); // Leer la dirección MAC de la interfaz WiFi STA

    if (ret != ESP_OK) { // Verificar si la lectura fue exitosa
        ESP_LOGE(TAG, "Error al leer MAC: %s", esp_err_to_name(ret)); // Imprimir mensaje de error si la lectura falla
        return false;
    }

    ESP_LOGI(TAG, "MAC WiFi STA: %02X:%02X:%02X:%02X:%02X:%02X", // Imprimir la dirección MAC en formato hexadecimal
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);

    return true;
}


bool check_mac_address() {
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error al leer MAC: %s", esp_err_to_name(ret));
        return false;
    }

    uint8_t expected_mac[] = EXPECTED_MAC;
    if (memcmp(mac, expected_mac, sizeof(mac)) != 0) {
        ESP_LOGE(TAG, "MAC no coincide. Firmware no autorizado.");
        return false;
    }

    ESP_LOGI(TAG, "MAC verificada correctamente.");
    return true;
}

void app_main(void)
{
    print_mac_address();
    // Verificar MAC antes de inicializar
    if (!check_mac_address()) {
        ESP_LOGE(TAG, "Firmware bloqueado por MAC no válida.");
        // Opcional: apagar o reiniciar
       // esp_restart();
        return;
    }

    init_PIN();
    // Inicializar NVS
    esp_err_t ret = nvs_flash_init();                                             // nvs es un sistema de almacenamiento no volátil que permite guardar datos en la memoria flash del ESP32. Se utiliza para almacenar configuraciones, credenciales y otros datos persistentes que deben conservarse incluso después de reiniciar el dispositivo.
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) // Si no hay páginas libres o se encuentra una nueva versión de NVS, se borra la memoria flash y se reinicia la inicialización de NVS
    {
        ESP_ERROR_CHECK(nvs_flash_erase()); // Borrar la memoria flash de NVS
        ret = nvs_flash_init();             // Reiniciar la inicialización de NVS
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();

    // Lanzar OTA
    create_tasks();
}
