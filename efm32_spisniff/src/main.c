#include <iot-os.h>
#include "debug.h"

#define DMA_SNIFF_CTRL \
        DMA_CTRL_DST_INC_BYTE|\
        DMA_CTRL_DST_SIZE_BYTE|\
        DMA_CTRL_DST_PROT_PRIVILEGED|\
        DMA_CTRL_SRC_INC_NONE|\
        DMA_CTRL_SRC_SIZE_BYTE|\
        DMA_CTRL_SRC_PROT_PRIVILEGED|\
        DMA_CTRL_R_POWER_1|\
        DMA_CTRL_CYCLE_CTRL_PINGPONG|\
        ((DMA_BUFFER_SIZE-1)<<_DMA_CTRL_N_MINUS_1_SHIFT);

#define SPISCREEN_WIDTH 128
#define SPISCREEN_HEIGHT 128
#define SPISCREEN_BUFWIDTH ((SPISCREEN_WIDTH+7)/8)
#define SPISCREEN_BUFSIZE (SPISCREEN_BUFWIDTH*SPISCREEN_HEIGHT)


#define GPIO_CS_PIN (1UL<<3)

typedef enum {
    SNSTATE_IDLE,
    SNSTATE_ACTIVE,
    SNSTATE_ROW,
    SNSTATE_DATA,
    SNSTATE_DONE,
    SNSTATE_ERROR
} TSniffState;

typedef struct {
    DMA_DESCRIPTOR_TypeDef pri[16];
    DMA_DESCRIPTOR_TypeDef alt[DMA_CHAN_COUNT];
} TDMActrl  __attribute__((aligned(0x100)));

static volatile TDMActrl g_dma_ctrl;
static volatile uint8_t g_pri_alt;
static volatile uint8_t g_screen_row, g_screen_col;
static volatile uint32_t g_cs_irq, g_buffer_head, g_buffer_tail;
static volatile uint32_t  g_sniff_errors;
static volatile TSniffState g_sniff_state, g_sniff_lasterror;
static uint8_t g_buffer[DMA_BUFFER_SIZE*2];
static volatile uint8_t g_screen[SPISCREEN_BUFSIZE];
static volatile uint8_t *g_screen_ptr, g_screen_x;

static void sniff_process(uint8_t data)
{
    switch(g_sniff_state)
    {
        /* waiting for first SPI data after CS */
        case SNSTATE_IDLE:
            if(data==0x80)
                g_sniff_state = SNSTATE_ROW;
            else
            {
                g_sniff_lasterror = g_sniff_state;
                g_sniff_state = SNSTATE_ERROR;
            }
            break;

        /* expecting row number  */
        case SNSTATE_ROW:
            if(data==0xFF)
                g_sniff_state = SNSTATE_DONE;
            else
            {
                if(!data || (data>SPISCREEN_WIDTH))
                {
                    g_sniff_lasterror = g_sniff_state;
                    g_sniff_state = SNSTATE_ERROR;
                }
                else
                {
                    g_screen_ptr = &g_screen[(data-1)*SPISCREEN_BUFWIDTH];
                    g_screen_x = 0;
                    g_sniff_state = SNSTATE_DATA;
                }
            }
            break;

        /* expecting display data  */
        case SNSTATE_DATA:
            *g_screen_ptr++ = data;
            g_screen_x++;
            if(g_screen_x>=SPISCREEN_BUFWIDTH)
                g_sniff_state = SNSTATE_ACTIVE;
            break;

        /* expecting next line within a CS'ed transaction  */
        case SNSTATE_ACTIVE:
            if(data==0xFF)
                g_sniff_state = SNSTATE_ROW;
            else
            {
                g_sniff_lasterror = g_sniff_state;
                g_sniff_state = SNSTATE_ERROR;
            }
            break;
    }
}

static void sniff_process_buffer(uint16_t head)
{
    uint8_t *p;
    g_buffer_head = head;

    p=&g_buffer[g_buffer_tail];
    while(g_buffer_tail!=head)
    {
        sniff_process(*p++);
        g_buffer_tail++;
        if(g_buffer_tail==(DMA_BUFFER_SIZE*2))
        {
            p = g_buffer;
            g_buffer_tail = 0;
        }
    }
}

static void sniff_gpio(void)
{
    volatile DMA_DESCRIPTOR_TypeDef *ch;
    uint32_t reason = GPIO->IF;
    uint32_t count;

    if(reason & GPIO_CS_PIN)
    {
        g_cs_irq++;

        /* reset sniff state */
        if(g_sniff_state!=SNSTATE_DONE)
            g_sniff_errors++;
        g_sniff_state = SNSTATE_IDLE;

        /* get current DMA channel */
        ch = g_pri_alt ?
            &g_dma_ctrl.alt[SPI_SNIFF_DMA_CH]:
            &g_dma_ctrl.pri[SPI_SNIFF_DMA_CH];

        /* calculate bytes in current DMA buffer */
        count = ((ch->CTRL & _DMA_CTRL_N_MINUS_1_MASK) >> _DMA_CTRL_N_MINUS_1_SHIFT);
        count = count ? ((DMA_BUFFER_SIZE-1)-count) : DMA_BUFFER_SIZE;

        /* process current buffer position */
        sniff_process_buffer((g_pri_alt*DMA_BUFFER_SIZE)+count);
    }

    /* acknowledge IRQ */
    GPIO->IFC = reason;
}

static void sniff_dma(void)
{
    volatile DMA_DESCRIPTOR_TypeDef *ch;
    uint8_t pri;
    uint32_t reason = DMA->IF;

    /* get current DMA channel */
    ch = g_pri_alt ?
        &g_dma_ctrl.alt[SPI_SNIFF_DMA_CH]:
        &g_dma_ctrl.pri[SPI_SNIFF_DMA_CH];

    /* re-activate channel */
    ch->CTRL = DMA_SNIFF_CTRL;

    /* process buffer */
    sniff_process_buffer(((g_pri_alt+1)*DMA_BUFFER_SIZE)-1);

    /* switch to next DMA buffer */
    g_pri_alt ^= 1;

    /* acknowledge IRQ */
    DMA->IFC = reason;
}

static inline void sniff_init(void)
{
    volatile DMA_DESCRIPTOR_TypeDef *ch;

    /* reset variables */
    g_cs_irq = 0;
    g_pri_alt = 0;
    g_screen_row = g_screen_col;
    g_buffer_head = g_buffer_tail = 0;
    g_sniff_state = SNSTATE_IDLE;

    /* Avoid false start by setting outputs as high */
    GPIO->P[3].DOUTCLR = 0xD;
    GPIO->P[3].MODEL =
        GPIO_P_MODEL_MODE0_INPUTPULL|
        GPIO_P_MODEL_MODE2_INPUTPULL|
        GPIO_P_MODEL_MODE3_INPUTPULL;

    /* start USART1 */
    CMU->HFPERCLKEN0 |= CMU_HFPERCLKEN0_USART1;

    USART1->ROUTE =
        USART_ROUTE_RXPEN |
        USART_ROUTE_CSPEN |
        USART_ROUTE_CLKPEN|
        USART_ROUTE_LOCATION_LOC1;
    USART1->CTRL = USART_CTRL_SYNC | USART_CTRL_CSINV;
    USART1->FRAME = USART_FRAME_DATABITS_EIGHT;
    USART1->CMD =
        USART_CMD_MASTERDIS |
        USART_CMD_RXEN |
        USART_CMD_TXTRIEN;

    /* setup DMA */
    DMA->CH[SPI_SNIFF_DMA_CH].CTRL =
        DMA_CH_CTRL_SOURCESEL_USART1|
        DMA_CH_CTRL_SIGSEL_USART1RXDATAV;

    /* setup primary DMA channel SPI_SNIFF_DMA_CH */
    ch = &g_dma_ctrl.pri[SPI_SNIFF_DMA_CH];
    ch->SRCEND = (void*)&USART1->RXDATA;
    ch->DSTEND = &g_buffer[DMA_BUFFER_SIZE-1];
    ch->CTRL = DMA_SNIFF_CTRL;
    ch->USER = (uint32_t)&g_buffer;

    /* setup alternate DMA channel SPI_SNIFF_DMA_CH */
    ch = &g_dma_ctrl.alt[SPI_SNIFF_DMA_CH];
    ch->SRCEND = (void*)&USART1->RXDATA;
    ch->DSTEND = &g_buffer[(DMA_BUFFER_SIZE*2)-1];
    ch->CTRL = DMA_SNIFF_CTRL;
    ch->USER = (uint32_t)&g_buffer[DMA_BUFFER_SIZE];

    /* start DMA */
    DMA->CHUSEBURSTS = 1<<SPI_SNIFF_DMA_CH;
    DMA->CHREQMASKC = 1<<SPI_SNIFF_DMA_CH;
    DMA->CHALTC = 1<<SPI_SNIFF_DMA_CH;
    DMA->CHENS = 1<<SPI_SNIFF_DMA_CH;
    DMA->IEN |= 1<<SPI_SNIFF_DMA_CH;
    DMA->RDS |= 1<<SPI_SNIFF_DMA_CH;

    /* init GPIO */
    GPIO->EXTIPSELL =
        (GPIO->EXTIPSELL & (~_GPIO_EXTIPSELL_EXTIPSEL3_MASK))|
        GPIO_EXTIPSELL_EXTIPSEL3_PORTD;
    GPIO->IEN = GPIO_CS_PIN;
    GPIO->EXTIFALL = GPIO_CS_PIN;
    ISR_SET(GPIO_ODD_IRQn, &sniff_gpio);
    NVIC_EnableIRQ(GPIO_ODD_IRQn);
}

#if 0
static inline void vga_init(void)
{
    /* enable time clock */
    CMU->HFPERCLKEN0 |= CMU_HFPERCLKEN0_TIMER0|CMU_HFPERCLKEN0_TIMER1;

    TIMER0->ROUTE = TIMER_ROUTE_LOCATION_LOC0
    TIMER0->TOP = SCREEN_HTOTAL;
    TIMER0->CTRL =

    TIMER1->TOP = SCREEN_VTOTAL;
    TIMER1->

}
#endif

static inline void hardware_init(void)
{
    /* Enable clocks for peripherals */
    CMU->HFPERCLKDIV = CMU_HFPERCLKDIV_HFPERCLKEN;
    CMU->HFPERCLKEN0 |= CMU_HFPERCLKEN0_GPIO;

    /* DIV4 clock for frequencies above 48MHz */
    CMU->HFCORECLKDIV = CMU_HFCORECLKDIV_HFCORECLKLEDIV;

    /* Set calibration for 28MHz band crystal */
    CMU->HFRCOCTRL = CMU_HFRCOCTRL_BAND_28MHZ |
        ((DEVINFO->HFRCOCAL1 & _DEVINFO_HFRCOCAL1_BAND28_MASK)
        >> _DEVINFO_AUXHFRCOCAL1_BAND28_SHIFT);
    while(!(CMU->STATUS & CMU_STATUS_HFRCORDY));

    /* Enable output */
    DEBUG_init();

    /* start DMA */
    CMU->HFCORECLKEN0 |= CMU_HFCORECLKEN0_DMA;
    DMA->CTRLBASE = (uint32_t)&g_dma_ctrl;
    /* enable DMA irq */
    ISR_SET(DMA_IRQn, &sniff_dma);
    NVIC_EnableIRQ(DMA_IRQn);

    /* Enable sniffer */
    sniff_init();

    /* start DMA */
    DMA->CONFIG = DMA_CONFIG_EN | DMA_CONFIG_CHPROT;
}

void main(void)
{
    uint32_t i;
    volatile uint32_t t;

    /* initialize hardware */
    hardware_init();

    i=0;
    while(true)
    {
        for(t=0;t<1000000;t++);
        dprintf("head=%03i g_cs_irq=%04i (%i)\r\n",g_buffer_head,g_cs_irq,i++);
    }
}