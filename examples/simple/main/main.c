
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "scan_manager.h"


static const char* TAG= "prober-scanner";
static scanner_t* scn;


static void callback(scanner_event_data_t* evt_data){


    ESP_LOGI(TAG, "source no %d, line number %d",evt_data->source_number,evt_data->line_number);

}



static int createScanner(){


    size_t size=scannerGetSize(2,2);


    scn = (scanner_t*) malloc(size);



    uint8_t gpio_no[]={22,23};
    uint32_t pwm_width[]={2000,2400};
    scanner_config_t config={  

                .total_gpio = 2,
                .gpio_no = gpio_no,
                .total_signals=2,
                .pwm_widths_array=pwm_width,  //2.0ms and 2.4ms   
                .tolerance=100,                 //100
                .min_width=1500,
                .cb=callback
    };


    return scannerCreate(scn,&config);

    


}



void app_main(void)
{

    
    

    createScanner();


    while(1){

        vTaskDelay(pdMS_TO_TICKS(10));
    }





}
