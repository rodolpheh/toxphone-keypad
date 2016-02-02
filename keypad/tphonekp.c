#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>

#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/time.h>

#define NB_COLUMN           3
#define NB_ROWS             5

#define GPIO_OUT_0          17
#define GPIO_OUT_1          27
#define GPIO_OUT_2          22
#define GPIO_IN_0           14
#define GPIO_IN_1           15
#define GPIO_IN_2           18
#define GPIO_IN_3           23
#define GPIO_IN_4           24

#define LOW                 0
#define HIGH                1

#define DEBOUNCE_TIME       300

/**
3.3V (NC) -  1 x x 2  - 5V (NC)
             3 x x 4  - 5V (NC)
             5 x x 6  - GND (NC)
             7 x o 8  - row1
 GND (NC) -  9 x o 10 - row2
     col1 - 11 o o 12 - row3
     col2 - 13 o x 14 - GND (NC)
     col3 - 15 o o 16 - row4
3.3V (NC) - 17 x o 18 - row5
            19 x x 20 - GND (NC)
            21 x x 22
            23 x x 24
 GND (NC) - 25 x x 26
**/

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rodolphe Houdas");
MODULE_DESCRIPTION("A simple driver for vintage phone keypads");
MODULE_VERSION("1.0");

// Define the mapping of the buttons
static unsigned short keyboardKeymap[] = {
    KEY_1, KEY_2, KEY_3,
    KEY_4, KEY_5, KEY_6,
    KEY_7, KEY_8, KEY_9,
    KEY_A, KEY_0, KEY_D,
    KEY_N, KEY_R, KEY_B
};

// Define the GPIO for the rows
static struct gpio gpio_rows[] = {
    { GPIO_IN_0, GPIOF_IN, "row_0" },
    { GPIO_IN_1, GPIOF_IN, "row_1" },
    { GPIO_IN_2, GPIOF_IN, "row_2" }, 
    { GPIO_IN_3, GPIOF_IN, "row_3" }, 
    { GPIO_IN_4, GPIOF_IN, "row_4" }, 
};

// Define the GPIO for the columns
static struct gpio gpio_col[] = {
    { GPIO_OUT_0, GPIOF_OUT_INIT_HIGH, "col_0" },
    { GPIO_OUT_1, GPIOF_OUT_INIT_HIGH, "col_1" },
    { GPIO_OUT_2, GPIOF_OUT_INIT_HIGH, "col_2" }, 
};

// Store the irq numbers
static int irq_pin[5];

// Used for the debounce
unsigned int last_interrupt_time = 0;
static uint64_t epochMilli;

// Define a structure for the input device
struct input_dev *input;


// millis() function
// Return the time spent in milliseconds since epochMilli (uint32_t)
static unsigned int millis (void) {
    struct timeval tv ;
    uint64_t now ;

    do_gettimeofday(&tv) ;
    now  = (uint64_t)tv.tv_sec * (uint64_t)1000 + (uint64_t)(tv.tv_usec / 1000) ;

    return (uint32_t)(now - epochMilli) ;
}


// findIndex() function. Find the index of an integer in an array of integer.
// Return an integer or -1 if the value has not been found.
// arguments :
//      - int a[]: array in where to search for the value
//      - int size: size of the array
//      - int value: value to search for
int FindIndex(int a[], int size, int value ) {
    int index = 0;
    
    while(index<size && a[index]!=value) index++;
    
    return ( index == size ? -1 : index );
}


// irq callback when one of the button is pressed
static irqreturn_t kpgpio_irq(int irq, void *dev_id) { 
    unsigned long flags;
    unsigned int interrupt_time = millis();
    struct gpio *dev = dev_id;
    int col = 0;
    int row = 0;
    int pin_value = 1;
    short key;
    
    // Disable hard interrupts
    local_irq_save(flags);
    
    // Debounce the button
    if (interrupt_time - last_interrupt_time < DEBOUNCE_TIME) {
        local_irq_restore(flags);
        return IRQ_HANDLED;
    }
    last_interrupt_time = interrupt_time;
    
    // Find which row have been triggered
    row = FindIndex(irq_pin, NB_ROWS, irq);
    
    for(col=0 ; col<NB_COLUMN ; col++) {
        // Set the column off...
        gpio_set_value(gpio_col[col].gpio, LOW);
        // ... measure the state of the GPIO that have been triggered ...
        pin_value = gpio_get_value(dev->gpio);
        // ... and set the column off.
        gpio_set_value(gpio_col[col].gpio, HIGH);
        
        // If the GPIO was low, we can deduce which button have been pressed
        if(pin_value == LOW) {
            key = keyboardKeymap[col + (row*NB_COLUMN)];
            input_report_key(input, key, 1);
            input_sync(input);
            input_report_key(input, key, 0);
            input_sync(input);
            break;            
        }
    }
    
    // restore hard interrupts
    local_irq_restore(flags);
    return IRQ_HANDLED;
}

// Initialization of the module
static int __init tphonekp_init(void) {
    int error;
    int i;
    struct timeval tv ;

    // Taking the time when the module have been loaded to avoid an overflow. epochMilli is used by millis()
    do_gettimeofday(&tv) ;
    epochMilli = (uint64_t)tv.tv_sec * (uint64_t)1000    + (uint64_t)(tv.tv_usec / 1000) ;
    
    printk(KERN_NOTICE "toxphone: starting keypad driver");    
    printk(KERN_NOTICE "toxphone: initializing GPIO");
    
    // Register GPIOs for columns
    error = gpio_request_array(gpio_col, NB_COLUMN);
    if(error) {
        printk(KERN_ALERT "toxphone: GPIO columns request array failed");
        goto err_return;
    }
    
    // Register GPIOs for rows
    error = gpio_request_array(gpio_rows, NB_ROWS);
    if(error) {
        printk(KERN_ALERT "toxphone: GPIO rows request array failed");
        goto err_free_columns;
    }
    
    // Initialize the interrupts
    for(i=0 ; i<NB_ROWS ; i++) {
        irq_pin[i] = gpio_to_irq(gpio_rows[i].gpio);
        printk(KERN_NOTICE "toxphone: irq mapped into %d\n", irq_pin[i]);
        if(request_irq(irq_pin[i], kpgpio_irq, (IRQF_TRIGGER_RISING), gpio_rows[i].label, &gpio_rows[i])) {
            printk(KERN_ALERT "toxphone: IRQ request %d failed\n", irq_pin[i]);
            i = i-1;
            error = -EBUSY;
            goto err_free_irq;
        }
    }
        
    // Allocate keyboard device in the system
    input = input_allocate_device();
    if (!input) {
        printk(KERN_ERR "toxphone: allocating input device : not enough memory\n");
        error = -ENOMEM;
        goto err_free_irq;
    }
    
    // Set the parameters for the keyboard
    input->name = "ToxPhone Keypad";
    input->keycode = keyboardKeymap;
    input->keycodesize = sizeof(unsigned short);
    input->keycodemax = NB_COLUMN * NB_ROWS;
    input->evbit[0] = BIT(EV_KEY);
    for (i = 0; i < NB_COLUMN * NB_ROWS; i++) {
        __set_bit(keyboardKeymap[i], input->keybit);
    }

    // Register the keyboard device
    error = input_register_device(input);
    if (error) {
        printk(KERN_CRIT "toxphone: failed to register device\n");
        goto err_free_dev;
    }
    
    printk(KERN_NOTICE "toxphone: keypad driver initialised\n");
    
    return 0;
    
    // Handling errors when initializing device, irqs or GPIOs
err_free_dev:
    input_free_device(input);
err_free_irq:
    for(i = i ; i == 0 ; i = i-1) { free_irq(irq_pin[i], &gpio_rows[i]); }
err_free_columns:
    gpio_free_array(gpio_col, NB_COLUMN);
err_return:
    return error;
        
}

// Exit of the module
static void __exit tphonekp_exit(void)
{
    int i;

    // Unregister and free the input device
    input_unregister_device(input);
    input_free_device(input);
    
    printk(KERN_NOTICE "toxphone: freeing GPIO");

    // Free irqs and GPIOs
    for(i=0 ; i<NB_ROWS ; i++) { free_irq(irq_pin[i], &gpio_rows[i]); }
    gpio_free_array(gpio_col, NB_COLUMN);
    gpio_free_array(gpio_rows, NB_ROWS);

    printk(KERN_NOTICE "toxphone: keypad driver exit");
}


module_init(tphonekp_init);
module_exit(tphonekp_exit);
