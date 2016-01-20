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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rodolphe Houdas");
MODULE_DESCRIPTION("A simple driver for vintage phone keypads");
MODULE_VERSION("0.2");

static struct gpio gpio_rows[] = {
    { GPIO_IN_0, GPIOF_IN, "row_0" },
    { GPIO_IN_1, GPIOF_IN, "row_1" },
    { GPIO_IN_2, GPIOF_IN, "row_2" }, 
    { GPIO_IN_3, GPIOF_IN, "row_3" }, 
    { GPIO_IN_4, GPIOF_IN, "row_4" }, 
};

static struct gpio gpio_col[] = {
    { GPIO_OUT_0, GPIOF_OUT_INIT_HIGH, "col_0" },
    { GPIO_OUT_1, GPIOF_OUT_INIT_HIGH, "col_1" },
    { GPIO_OUT_2, GPIOF_OUT_INIT_HIGH, "col_2" }, 
};

static int irq_pin[5];

unsigned int last_interrupt_time = 0;
static uint64_t epochMilli;

// Define a structure for the input device
struct input_dev *input;

static unsigned short keyboardKeymap[] = {
    KEY_1, KEY_2, KEY_3,
    KEY_4, KEY_5, KEY_6,
    KEY_7, KEY_8, KEY_9,
    KEY_A, KEY_0, KEY_D,
    KEY_N, KEY_R, KEY_B
};

static unsigned int millis (void)
{
  struct timeval tv ;
  uint64_t now ;

  do_gettimeofday(&tv) ;
  now  = (uint64_t)tv.tv_sec * (uint64_t)1000 + (uint64_t)(tv.tv_usec / 1000) ;

  return (uint32_t)(now - epochMilli) ;
}

int FindIndex(int a[], int size, int value ) {
    int index = 0;
    while(index<size && a[index]!=value) index++;
    return ( index == size ? -1 : index );
}

static irqreturn_t kpgpio_irq(int irq, void *dev_id) { 
    
    unsigned long flags;
    unsigned int interrupt_time = millis();
    
    struct gpio *dev = dev_id;
    
    int col = 0;
    int row;
    int pin_value = 1;
    short key;
    
    // disable hard interrupts (remember them in flag 'flags')
    local_irq_save(flags);

    if (interrupt_time - last_interrupt_time < 300) {
        local_irq_restore(flags);
        return IRQ_HANDLED;
    }
    last_interrupt_time = interrupt_time;
    
    row = FindIndex(irq_pin, NB_ROWS, irq);
    printk(KERN_NOTICE "%i", row);
    
    for(col=0 ; col<NB_COLUMN ; col++) {
        gpio_set_value(gpio_col[col].gpio, LOW);
        pin_value = gpio_get_value(dev->gpio);
        gpio_set_value(gpio_col[col].gpio, HIGH);
        
        if(pin_value == LOW) {
            key = keyboardKeymap[col + (row*NB_COLUMN)];
            input_report_key(input, key, 1);
            input_sync(input);
            input_report_key(input, key, 0);
            input_sync(input);
            local_irq_restore(flags);
            return IRQ_HANDLED;            
        }
    }
    
    // restore hard interrupts
    local_irq_restore(flags);
    
    return IRQ_HANDLED;
}

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
    if(gpio_request_array(gpio_col, NB_COLUMN)) {
        printk(KERN_ALERT "toxphone: GPIO columns request array failed");
    }
    
    // Register GPIOs for rows
    if(gpio_request_array(gpio_rows, NB_ROWS)) {
        printk(KERN_ALERT "toxphone: GPIO rows request array failed");
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
    
    // Handling errors when initializing device or irqs
err_free_dev:
    input_free_device(input);
err_free_irq:
    for(i = i ; i == 0 ; i = i-1) {
        free_irq(irq_pin[i], &gpio_rows[i]);
    }
    return error;
        
}

static void __exit tphonekp_exit(void)
{
    int i;

    input_unregister_device(input);
    input_free_device(input);
    
    printk(KERN_NOTICE "toxphone: freeing GPIO");

    for(i=0 ; i<NB_ROWS ; i++) {
        free_irq(irq_pin[i], &gpio_rows[i]);
    }
    
    gpio_free_array(gpio_col, NB_COLUMN);
    gpio_free_array(gpio_rows, NB_ROWS);

    printk(KERN_NOTICE "toxphone: keypad driver exit");
}

module_init(tphonekp_init);
module_exit(tphonekp_exit);
