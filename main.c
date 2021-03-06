/*
 * STM32F4 board support for the bootloader.
 *
 */

#include "hw_config.h"

#include <stdlib.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/pwr.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/cm3/scb.h>

#include <libopencmsis/core_cm3.h>

#include "bl.h"
#include <string.h>
#include "img.h"

/* flash parameters that we should not really know */
static struct {
	uint32_t	sector_number;
	uint32_t	size;
} flash_sectors[] = {

	/* Physical FLASH sector 0 is reserved for bootloader and is not
	 * the table below.
	 * N sectors may aslo be reserved for the app fw in which case
	 * the zero based define BOARD_FIRST_FLASH_SECTOR_TO_ERASE must
	 * be defined to begin the erase above of the reserved sectors.
	 * The default value of BOARD_FIRST_FLASH_SECTOR_TO_ERASE is 0
	 * and begins flash erase operations at phsical sector 1 the 0th entry
	 * in the table below.
	 * A value of 1 for BOARD_FIRST_FLASH_SECTOR_TO_ERASE would reserve
	 * the 0th entry and begin erasing a index 1 the third physical sector
	 * on the device.
	 *
	 * When BOARD_FIRST_FLASH_SECTOR_TO_ERASE is defined APP_RESERVATION_SIZE
	 * must also be defined to remove that additonal reserved FLASH space
	 * from the BOARD_FLASH_SIZE. See APP_SIZE_MAX below.
	 */

	{0x01, 16 * 1024},
	{0x02, 16 * 1024},
	{0x03, 16 * 1024},
	{0x04, 64 * 1024},
	{0x05, 128 * 1024},
	{0x06, 128 * 1024},
	{0x07, 128 * 1024},
	{0x08, 128 * 1024},
	{0x09, 128 * 1024},
	{0x0a, 128 * 1024},
	{0x0b, 128 * 1024},
	/* flash sectors only in 2MiB devices */
	{0x10, 16 * 1024},
	{0x11, 16 * 1024},
	{0x12, 16 * 1024},
	{0x13, 16 * 1024},
	{0x14, 64 * 1024},
	{0x15, 128 * 1024},
	{0x16, 128 * 1024},
	{0x17, 128 * 1024},
	{0x18, 128 * 1024},
	{0x19, 128 * 1024},
	{0x1a, 128 * 1024},
	{0x1b, 128 * 1024},
};
#define BOOTLOADER_RESERVATION_SIZE	(16 * 1024)

#define OTP_BASE			0x1fff7800
#define OTP_SIZE			512
#define UDID_START		        0x1FFF7A10

// address of MCU IDCODE
#define DBGMCU_IDCODE		0xE0042000
#define STM32_UNKNOWN	0
#define STM32F40x_41x	0x413
#define STM32F42x_43x	0x419
#define STM32F42x_446xx	0x421

#define REVID_MASK	0xFFFF0000
#define DEVID_MASK	0xFFF

#ifndef BOARD_PIN_VBUS
# define BOARD_PIN_VBUS                 GPIO9
# define BOARD_PORT_VBUS                GPIOA
# define BOARD_CLOCK_VBUS               RCC_AHB1ENR_IOPAEN
#endif

/* magic numbers from reference manual */

typedef enum mcu_rev_e {
	MCU_REV_STM32F4_REV_A = 0x1000,
	MCU_REV_STM32F4_REV_Z = 0x1001,
	MCU_REV_STM32F4_REV_Y = 0x1003,
	MCU_REV_STM32F4_REV_1 = 0x1007,
	MCU_REV_STM32F4_REV_3 = 0x2001
} mcu_rev_e;

typedef struct mcu_des_t {
	uint16_t mcuid;
	const char *desc;
	char  rev;
} mcu_des_t;

// The default CPU ID  of STM32_UNKNOWN is 0 and is in offset 0
// Before a rev is known it is set to ?
// There for new silicon will result in STM32F4..,?
mcu_des_t mcu_descriptions[] = {
	{ STM32_UNKNOWN,	"STM32F???",    '?'},
	{ STM32F40x_41x, 	"STM32F40x",	'?'},
	{ STM32F42x_43x, 	"STM32F42x",	'?'},
	{ STM32F42x_446xx, 	"STM32F446XX",	'?'},
};

char serial_number[32];
#define STM32_UUID ((uint32_t *)UDID_START)

unsigned bootFlag = 0;

static void initSerialNumber()
{
	writeHex(serial_number, STM32_UUID[0]);
	writeHex(serial_number+8, STM32_UUID[1]);
	writeHex(serial_number+16, STM32_UUID[2]);
}


typedef struct mcu_rev_t {
	mcu_rev_e revid;
	char  rev;
} mcu_rev_t;

/* context passed to cinit */
#if INTERFACE_USB
# define BOARD_INTERFACE_CONFIG_USB  	NULL
#endif

/* board definition */
struct boardinfo board_info = {
	.board_rev	= 0,

#ifdef STM32F401
	.systick_mhz	= 84,
#else
	.systick_mhz	= 168,
#endif
};

static void board_init(void);

#define BOOT_RTC_SIGNATURE          0x71a21877
#define APP_RTC_SIGNATURE           0x24a22d12
#define POWER_DOWN_RTC_SIGNATURE    0x5019684f // Written by app fw to not re-power on.
#define HF2_RTC_SIGNATURE           0x39a63a78
#define SLEEP_RTC_ARG               0x10b37889
#define SLEEP2_RTC_ARG              0x7e3353b7

#define BOOT_RTC_REG                MMIO32(RTC_BASE + 0x50)
#define ARG_RTC_REG                MMIO32(RTC_BASE + 0x54)

/* standard clocking for all F4 boards */
static struct rcc_clock_scale clock_setup = {
	.pllm = 0,
	.plln = 336,
#if defined(STM32F401)
	.pllp = 4,
	.pllq = 7,
	.pllr = 0,
	.hpre = RCC_CFGR_HPRE_DIV_NONE,
	.ppre1 = RCC_CFGR_PPRE_DIV_2,
	.ppre2 = RCC_CFGR_PPRE_DIV_NONE,
	.flash_config = FLASH_ACR_ICE | FLASH_ACR_DCE | FLASH_ACR_LATENCY_2WS,
	.ahb_frequency  = 84000000,
#else
	.pllp = 2,
	.pllq = 7,
#if defined(STM32F446) || defined(STM32F469)
	.pllr = 2,
#endif
	.hpre = RCC_CFGR_HPRE_DIV_NONE,
	.ppre1 = RCC_CFGR_PPRE_DIV_4,
	.ppre2 = RCC_CFGR_PPRE_DIV_2,
	.flash_config = FLASH_ACR_ICE | FLASH_ACR_DCE | FLASH_ACR_LATENCY_5WS,
#endif
	.power_save = 0,
	.apb1_frequency = 42000000,
	.apb2_frequency = 84000000,
};

static uint32_t
board_get_rtc_signature(uint32_t *arg)
{
	/* enable the backup registers */
	PWR_CR |= PWR_CR_DBP;
	RCC_BDCR |= RCC_BDCR_RTCEN;

	uint32_t result = BOOT_RTC_REG;
	if (arg)
		*arg = ARG_RTC_REG;

	/* disable the backup registers */
	RCC_BDCR &= RCC_BDCR_RTCEN;
	PWR_CR &= ~PWR_CR_DBP;

	return result;
}

void
board_set_rtc_signature(uint32_t sig, uint32_t arg)
{
	/* enable the backup registers */
	PWR_CR |= PWR_CR_DBP;
	RCC_BDCR |= RCC_BDCR_RTCEN;

	BOOT_RTC_REG = sig;
	ARG_RTC_REG = arg;

	/* disable the backup registers */
	RCC_BDCR &= RCC_BDCR_RTCEN;
	PWR_CR &= ~PWR_CR_DBP;
}

static bool
board_test_force_pin()
{
#if defined(BOARD_FORCE_BL_PIN_IN) && defined(BOARD_FORCE_BL_PIN_OUT)
	/* two pins strapped together */
	volatile unsigned samples = 0;
	volatile unsigned vote = 0;

	for (volatile unsigned cycles = 0; cycles < 10; cycles++) {
		gpio_set(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN_OUT);

		for (unsigned count = 0; count < 20; count++) {
			if (gpio_get(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN_IN) != 0) {
				vote++;
			}

			samples++;
		}

		gpio_clear(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN_OUT);

		for (unsigned count = 0; count < 20; count++) {
			if (gpio_get(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN_IN) == 0) {
				vote++;
			}

			samples++;
		}
	}

	/* the idea here is to reject wire-to-wire coupling, so require > 90% agreement */
	if ((vote * 100) > (samples * 90)) {
		return true;
	}

#endif
#if defined(BOARD_FORCE_BL_PIN)
	/* single pin pulled up or down */
	volatile unsigned samples = 0;
	volatile unsigned vote = 0;

	for (samples = 0; samples < 200; samples++) {
		if ((gpio_get(BOARD_FORCE_BL_PORT, BOARD_FORCE_BL_PIN) ? 1 : 0) == BOARD_FORCE_BL_STATE) {
			vote++;
		}
	}

	/* reject a little noise */
	if ((vote * 100) > (samples * 90)) {
		return true;
	}

#endif
	return false;
}


static void
board_init(void)
{
	RCC_APB1ENR |= RCC_APB1ENR_PWREN;
	RCC_APB2ENR |= RCC_APB2ENR_SYSCFGEN;
	
	// enable all GPIO clocks
	RCC_AHB1ENR |= RCC_AHB1ENR_IOPAEN|RCC_AHB1ENR_IOPBEN|RCC_AHB1ENR_IOPCEN|BOARD_CLOCK_VBUS;

	// make sure JACDAC line is up, otherwise trashes the bus
	setup_input_pin(CFG_PIN_BTN_LEFT); // use left to detect bootloader mode

	setup_input_pin(CFG_PIN_JACK_TX);

	setup_output_pin(CFG_PIN_LED);
	setup_output_pin(CFG_PIN_LED1);

	setup_output_pin(CFG_PIN_JACK_SND);


	initSerialNumber();

	start_systick();
}

static void initSpi(){
    // spi2 pin pb12~15
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_SPI2);
    setup_output_pin(CFG_PIN_FLASH_CS);
    pin_set(CFG_PIN_FLASH_CS, 1);

    gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO13 | GPIO14 | GPIO15);
    gpio_set_af(GPIOB, GPIO_AF5, GPIO13 | GPIO14 | GPIO15);

    spi_reset(SPI2);
    spi_init_master(SPI2, SPI_CR1_BAUDRATE_FPCLK_DIV_4, SPI_CR1_CPOL_CLK_TO_1_WHEN_IDLE,
                    SPI_CR1_CPHA_CLK_TRANSITION_2, SPI_CR1_DFF_8BIT, SPI_CR1_MSBFIRST);
    spi_enable_software_slave_management(SPI2);
    spi_set_nss_high(SPI2);
    spi_enable(SPI2);
    DMESG("SPI2 init");
}

/**
  * @brief  Initializes the RCC clock configuration.
  *
  * @param  clock_setup : The clock configuration to set
  */
static inline void
clock_init(void)
{
	uint32_t pllm = BOOT_SETTINGS->hseValue / 1000000;
	if (pllm < 4 || pllm > 60 || pllm * 1000000 != BOOT_SETTINGS->hseValue)
		pllm = OSC_FREQ;
	clock_setup.pllm = pllm;
	rcc_clock_setup_hse_3v3(&clock_setup);
}

void
led_on(unsigned led)
{
	switch (led) {
	case LED_ACTIVITY:
		pin_set(CFG_PIN_LED, 1);
		break;

	case LED_BOOTLOADER:
		pin_set(CFG_PIN_LED1, 1);
		break;
	}
}

void
led_off(unsigned led)
{
	switch (led) {
	case LED_ACTIVITY:
		pin_set(CFG_PIN_LED, 0);
		break;

	case LED_BOOTLOADER:
		pin_set(CFG_PIN_LED1, 0);
		break;
	}
}

/* we should know this, but we don't */
#ifndef SCB_CPACR
# define SCB_CPACR (*((volatile uint32_t *) (((0xE000E000UL) + 0x0D00UL) + 0x088)))
#endif

#define PWR_CR_LPLVDS (1 << 10)

void playTone()
{
	for (int i=0;i<100;i++)
	{
		pin_set(CFG_PIN_JACK_SND, 1);
		delay(1);
		pin_set(CFG_PIN_JACK_SND, 0);
		delay(1);
	}	
}

int
main(void)
{
	/* Enable the FPU before we hit any FP instructions */
	SCB_CPACR |= ((3UL << 10 * 2) | (3UL << 11 * 2)); /* set CP10 Full Access and set CP11 Full Access */

	/* do board-specific initialisation */
	board_init();

	/* configure the clock for bootloader activity */
	clock_init();

	initSpi();

	screen_init();
	//draw_drag();

	playTone();

	// if they hit reset the second time, go to app
	board_set_rtc_signature(APP_RTC_SIGNATURE, 0);
	board_set_rtc_signature(0, 0);
	int x=0;
	int y=0;
	bool right=true;
	bool down=true;
	while (1) {
		if (x==0)
			right=true;
		else if (x==100)
		    right=false;
		
		if (y==0)
			down=true;
		else if (y==80)
		    down=false;

		if (right)
		{
			//drawImage(x,0,45,45,emptyImg);
			drawImage(++x,0,45,45,hamImg);
		}
		else
		{ 
			//drawImage(x,0,45,45,emptyImg);
			drawImage(--x,0,45,45,hamImg);
		}
		if (down)
		{ 
			//drawImage(0,y,45,45,emptyImg);
			drawImage(0,++y,45,45,hamImg);
		}
		else
		{ 
			//drawImage(0,y,45,45,emptyImg);
			drawImage(0,--y,45,45,hamImg);
		}
		
		//led_on(1);
		//led_off(2);
		//delay(100);
		//led_off(1);
		//led_on(2);
		//delay(1);
	}
}

