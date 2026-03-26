/*
 * Recetor Infravermelho - Fase 2
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

#define LCD_I2C_ADDRESS 0x27  
#define SDA_PIN 8
#define SCL_PIN 9
#define IR_RX_PIN 4

static const char *TAG = "RECETOR";

// ==========================================
// FUNÇÕES DO ECRÃ LCD (Iguais ao Emissor)
// ==========================================
void lcd_write_nibble(uint8_t nibble, uint8_t mode) {
    uint8_t data = nibble | mode | 0x08; 
    uint8_t off = data & ~0x04;          
    uint8_t on = data | 0x04;            
    uint8_t pkt[3] = {off, on, off}; 
    i2c_master_write_to_device(I2C_NUM_0, LCD_I2C_ADDRESS, pkt, 3, pdMS_TO_TICKS(1000));
    esp_rom_delay_us(200); 
}

void lcd_write_byte(uint8_t val, uint8_t mode) {
    lcd_write_nibble(val & 0xF0, mode);      
    lcd_write_nibble((val << 4) & 0xF0, mode); 
}

void lcd_put_str(const char *str) {
    while (*str) {
        lcd_write_byte(*str++, 0x01); 
        esp_rom_delay_us(2000); 
    }
}

void lcd_init() {
    vTaskDelay(pdMS_TO_TICKS(100));
    lcd_write_nibble(0x30, 0); 
    vTaskDelay(pdMS_TO_TICKS(5));
    lcd_write_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_write_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_write_nibble(0x20, 0); 
    lcd_write_byte(0x28, 0); 
    lcd_write_byte(0x0C, 0); 
    lcd_write_byte(0x01, 0); 
    vTaskDelay(pdMS_TO_TICKS(20));
    lcd_write_byte(0x06, 0); 
}

// ==========================================
// FUNÇÕES DO RECETOR RMT E DESCODIFICAÇÃO
// ==========================================
SemaphoreHandle_t rx_sem = NULL;

// Callback: Ouve um disparo de luz e avisa o loop principal
bool rmt_rx_done_cb(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data) {
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(rx_sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

void app_main(void) {
    // 1. Iniciar o I2C e o Ecrã LCD
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 50000,
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);

    lcd_init();
    lcd_put_str("A aguardar");
    lcd_write_byte(0xC0, 0); // Salta para a linha de baixo
    lcd_put_str("mensagem...");

    // 2. Iniciar Recetor RMT no GPIO 4
    rmt_channel_handle_t rx_channel = NULL;
    rmt_rx_channel_config_t rx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, // Lê o tempo em microsegundos
        .mem_block_symbols = 64,
        .gpio_num = IR_RX_PIN,
        .flags.invert_in = true, // MAGIA: O VS1838B é Active-Low, isto inverte-o logo para lógica normal!
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_chan_config, &rx_channel));

    rx_sem = xSemaphoreCreateBinary();
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rmt_rx_done_cb };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(rx_channel));

    // Regras: Ignorar luzes parasitas muito rápidas e esperar 12ms para dar a mensagem como terminada
    rmt_receive_config_t rec_config = {
        .signal_range_min_ns = 2000,   
        .signal_range_max_ns = 12000000, 
    };

    // --- A CORREÇÃO DO REBOOT ESTÁ AQUI (A palavra static salva a memória) ---
    static rmt_symbol_word_t raw_symbols[1000]; 

    ESP_LOGI(TAG, "Recetor Ativo e à escuta no GPIO %d!", IR_RX_PIN);

    // 3. O Loop Principal do Detetive (À espera da luz)
    while (1) {
        memset(raw_symbols, 0, sizeof(raw_symbols)); // Limpa o "saco"
        
        // Ativa o ouvido do ESP32
        if (rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &rec_config) == ESP_OK) {
            
            // Fica aqui parado até a luz parar de piscar (mensagem completa recebida)
            if (xSemaphoreTake(rx_sem, portMAX_DELAY) == pdTRUE) {
                
                int start_idx = -1;
                
                // Passo 1: Procurar a "Chave" de START (Aprox 9000us ligado e 4500us desligado)
                for (int i = 0; i < 1000; i++) {
                    if (raw_symbols[i].duration0 > 8000 && raw_symbols[i].duration0 < 10000 &&
                        raw_symbols[i].duration1 > 4000 && raw_symbols[i].duration1 < 5000) {
                        start_idx = i + 1; // Encontrou! A mensagem começa no índice seguinte
                        break;
                    }
                }

                if (start_idx != -1) {
                    char msg[61] = {0};
                    int bit_count = 0;
                    int char_idx = 0;
                    char current_char = 0;

                    // Passo 2: Transformar a luz em Letras
                    for (int i = start_idx; i < 1000; i++) {
                        if (raw_symbols[i].duration0 == 0) break; // Acabaram-se os dados

                        // É um pulso de luz de 560us válido?
                        if (raw_symbols[i].duration0 > 400 && raw_symbols[i].duration0 < 750) {
                            
                            if (raw_symbols[i].duration1 > 1500 && raw_symbols[i].duration1 < 1900) {
                                // Foi uma pausa longa: é um '1' lógico!
                                current_char |= (1 << bit_count);
                                bit_count++;
                            } else if (raw_symbols[i].duration1 > 400 && raw_symbols[i].duration1 < 750) {
                                // Foi uma pausa curta: é um '0' lógico!
                                bit_count++;
                            } else {
                                break; // Bateu no final
                            }

                            // Fez 8 bits? Então temos 1 letra completa!
                            if (bit_count == 8) {
                                msg[char_idx++] = current_char;
                                current_char = 0;
                                bit_count = 0;
                                if (char_idx == 60) break; // Bateu no limite
                            }
                        } else {
                            break; // Foi ruído ou interferência da luz do sol
                        }
                    }

                    // Passo 3: Se conseguiu traduzir a luz, escarrapacha no LCD!
                    if (char_idx > 0) {
                        msg[char_idx] = '\0'; // Fecha a string
                        ESP_LOGI(TAG, "Mensagem secreta intercetada: %s", msg);
                        
                        lcd_write_byte(0x01, 0); // Limpa o LCD
                        vTaskDelay(pdMS_TO_TICKS(20));
                        
                        // Imprime de forma inteligente dependendo do tamanho da mensagem
                        if(char_idx <= 20) {
                            lcd_put_str(msg);
                        } else {
                            char temp[21];
                            strncpy(temp, msg, 20); // Primeira linha
                            temp[20] = '\0';
                            lcd_put_str(temp);
                            
                            lcd_write_byte(0xC0, 0); // Vai para a segunda linha
                            lcd_put_str(msg + 20);
                        }
                    }
                }
            }
        } else {
            // A OUTRA CORREÇÃO: Evita que a placa congele se a escuta falhar
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}