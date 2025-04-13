#include <string.h>
#include "esp_log.h"
#include "pwm_capture.h"
#include "scan_manager.h"



static const char* TAG = "scan manager";
#define         MAX_CHANNELS                6
#define         MAX_CHANNELS_PER_UNIT       3






#define CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))





#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})


static int startScanning(scanner_interface_t* self){

    scanner_t* scanner = container_of(self,scanner_t,interface);


    uint8_t total_lines=scanner->total_lines;
    ESP_LOGI(TAG,"got u %d",total_lines);


    
    for(uint8_t i=0; i<total_lines ; i++){

        scanner->list->interface.startMonitoring(&scanner->list[i].interface);

    }
    
    return 0;    
}



size_t scannerGetSize(uint8_t total_gpio,uint8_t total_signals){
    if(total_gpio==0 || total_signals==0)
        return -1;


    //This is for the pulse scanner instance
    size_t size=sizeof(scanner_t);
    //The capture lines , 4 for a n*4 keypad
    size+=sizeof(pwm_capture_t)*total_gpio;


    //This is for the class data shared by all the pwm_capture objects
    //Class data will be separate for each
    size+=sizeof(pwm_capture_class_data_t);

    /*The internals of the class_data are not included bcz they will be allocated using malloc
        Because iit is a never ending chain of getting sizes
    */

    //The total signals , 4 for a 4*n keypad
    //size+=sizeof(uint32_t)*total_signals;

    //size+

    return size;
}




int scannerCreate(scanner_t* self,scanner_config_t* config){

    if(self==NULL || config==NULL)
        return ERR_SCANNER_INVALID_MEM;
    
    uint8_t total_gpio=config->total_gpio; 
    uint8_t* gpio_no=config->gpio_no;
    uint8_t total_signals=config->total_signals;
    uint32_t* pwm_widths_array=config->pwm_widths_array;
    uint32_t tolerance=config->tolerance;
    uint32_t min_width=config->min_width;
    callbackForScanner cb=config->cb;



    
    //Assign the memory to the pointers, list member gets the memory after the struct
    //self->list=(pwm_capture_t*)((uint8_t*)self + sizeof(scanner_t));

    self->list=(pwm_capture_t*) malloc(sizeof(pwm_capture_t)*total_gpio);
    //The class data gets the memory  after the list
    pwm_capture_class_data_t* class_data=(pwm_capture_class_data_t*) malloc(sizeof(pwm_capture_class_data_t));



    self->total_lines=total_gpio;
    self->interface.callback=cb;
    self->interface.startScanning=startScanning;
    
    
    
    //Class data init of capture object
    //Restart if error
    int ret=0;
    ESP_ERROR_CHECK(captureClassDataInit(class_data,min_width, tolerance,pwm_widths_array,total_gpio,total_signals, cb));

    self->class_data=class_data;
    /*
    if(ret!=0)
        return ret;
    */

    //Create all the instances of the capture objects now

    for(uint8_t i=0;i<total_gpio;i++){
        
        //The captureCreate function requires that
        memset(&self->list[i],0,sizeof(pwm_capture_t));
        //Assign each member of the list which is pwm_capture_t type
        self->list[i].class_data=class_data;
        ESP_ERROR_CHECK(captureCreate(&self->list[i],class_data,gpio_no[i]));
        
        /*
        if(ret!=0)
            return ret;*/
    }


    return 0;

}



