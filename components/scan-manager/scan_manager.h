#ifndef SCANNER_H
#define SCANNER_H

#include "pwm_capture.h"
#include "capture_event_data.h"




#define ERR_SCANNER_BASE                    0
#define ERR_SCANNER_INVALID_MEM             (ERR_SCANNER_BASE-1)



typedef struct scanner_interface{
   // void (*addCaptureLine)(scanner_interface_t* self,pwm_capture_t* line);
    int (*startScanning)(struct scanner_interface* self);
    int (*stopScanning)(struct scanner_interface* self);
    /*So the callback does not have self referencing because it will be defined by the user of this, 
        and purpose of self referencing is to access the private members of an instance which are
        different for each because they are also at different unique addresses
    */
    void (*callback)(scanner_event_data_t* event_data);
}scanner_interface_t;


typedef struct pulse_scanner{
    uint8_t total_lines;
    pwm_capture_class_data_t* class_data;    //data share among all the members
    pwm_capture_t* list;                    //Initialized internally
    scanner_interface_t interface;
}scanner_t;




//typedef struct pulse_scanner scanner_t;



//This is defined separately just so that the scannerCreate callback parameter is simple
typedef void (*callbackForScanner)(scanner_event_data_t* event_data);




typedef struct scanner_config{
    uint8_t total_gpio; 
    uint8_t* gpio_no;
    uint8_t total_signals;
    uint32_t* pwm_widths_array;
    uint32_t tolerance;
    uint32_t min_width;
    callbackForScanner cb;
}scanner_config_t;


//not used bcz struct shard in header
//size_t scannerGetSize(uint8_t total_gpio, uint8_t total_signals);
int scannerCreate(scanner_t* self,scanner_config_t* config);







#endif