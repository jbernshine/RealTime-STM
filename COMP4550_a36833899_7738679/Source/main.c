#include "main.h"
#include "stm32f4xx_conf.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "croutine.h"
#include "semphr.h"
#include "timers.h"
#include "codec.h"

#define STACK_SIZE_MIN 128
#define CLOCK_SPEED 84000000
#define PRESCALER 41999

#define BLUE_MAX_COUNT	4

#define NUM_TIMERS	5
#define NUM_TASKS 5
#define NUM_SCHED_ALGORITHMS 3

//Function prototypes
void vLongPressEvent(void* params);
void vDoublePressEvent(void* params);

#define MS_TO_FORLOOP_ITERATIONS 50000

typedef enum Boolean
{
	false,
	true,
} boolean;

//Tasks


typedef struct 
{
	int wcet;
	int timeCooked;
	int period;
	int priority;
	int deadline;
	uint16_t led;
	float freq;
} Pizza;

typedef struct
{
	Pizza * pizza;
	char * taskName;
	TaskHandle_t taskHandle;
	TimerHandle_t timer;
	SemaphoreHandle_t semaphore;
} TaskData;

typedef enum PizzaType
{
	HAWAIIAN,
	VEGGIE,
	PEPPERONI,
	SEAFOOD,
	NUM_PIZZAS,
} PizzaType;

Pizza pizzas[NUM_PIZZAS] = {
	[HAWAIIAN] = {
		.wcet = 6, 
		.period = 40, 
		.priority = 2, 
		.deadline = 10, 
		.led = GPIO_Pin_13, 
		.freq = 0.011 
	},
	[VEGGIE] = {
		.wcet = 4, 
		.period = 30, 
		.priority = 1, 
		.deadline = 10, 
		.led = GPIO_Pin_12, 
		.freq = 0.013 
	},	
	[PEPPERONI] = {
		.wcet = 8, 
		.period = 40, 
		.priority = 2, 
		.deadline = 15, 
		.led = GPIO_Pin_14, 
		.freq = 0.015 
	},
	[SEAFOOD] = { 
		.wcet = 3, 
		.period = 20, 
		.priority = 3, 
		.deadline = 5, 
		.led = GPIO_Pin_15, 
		.freq = 0.017 
	},
};

/*Scheduling algorithms*/
PizzaType fixedPriority(PizzaType pizza1, PizzaType pizza2);
PizzaType earliestDeadlineFirst(PizzaType pizza1, PizzaType pizza2);
PizzaType leastLaxityFirst(PizzaType pizza1, PizzaType pizza2);

PizzaType (*schedulingAlgorithms[NUM_SCHED_ALGORITHMS])(PizzaType pizza1, PizzaType pizza2) = {
	fixedPriority,
	earliestDeadlineFirst,
	leastLaxityFirst,
};

TaskData pizzaTasks[NUM_PIZZAS] = {
	[HAWAIIAN] = { .taskName = "Hawaiian" },
	[VEGGIE] = { .taskName = "Veggie" },
	[PEPPERONI] = { .taskName = "Pepperoni" },
	[SEAFOOD] = { .taskName = "Seafood" },
};

//Times in miliseconds
#define BOUNCE_THRESHOLD 30
#define PIZZA_COUNTDOWN_PERIOD 1000
#define LONG_PRESS_THRESHOLD 500
#define SOUND_DURATION 500

/********************************
 * File Variables
 *******************************/
volatile boolean isDoublePress = false;
volatile boolean isCooking = false;
unsigned int timeElapsed;
fir_8 filt;
SemaphoreHandle_t audioSemaphore;
SemaphoreHandle_t missedDeadlineSemaphore;
xTimerHandle xButtonTimer;
xTimerHandle xDoublePressTimer;
PizzaType scheduledPizza;
PizzaType(*schedulingDisciplineCompare) (PizzaType, PizzaType);
QueueHandle_t soundQueue;
int selectedAlgorithm = -1;
int missedDeadlines[NUM_PIZZAS];

/********************************
 * Hardware initialization
 *******************************/
 
void InitLeds()
{
	GPIO_InitTypeDef GPIO_Initstructure;
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);
	
 	GPIO_Initstructure.GPIO_Pin = GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15;
	GPIO_Initstructure.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_Initstructure.GPIO_OType = GPIO_OType_PP;	
	GPIO_Initstructure.GPIO_PuPd = GPIO_PuPd_DOWN;
	GPIO_Initstructure.GPIO_Speed = GPIO_Speed_100MHz;
	GPIO_Init(GPIOD, &GPIO_Initstructure);
}

void debug()
{
	GPIO_SetBits(GPIOD, 0xFFFF);
}

void InitButton()
{
	GPIO_InitTypeDef GPIO_Initstructure;
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE); 
	
	GPIO_Initstructure.GPIO_Mode = GPIO_Mode_IN;
	GPIO_Initstructure.GPIO_OType = GPIO_OType_PP;
	GPIO_Initstructure.GPIO_Pin = GPIO_Pin_0;
	GPIO_Initstructure.GPIO_PuPd = GPIO_PuPd_DOWN;
	GPIO_Initstructure.GPIO_Speed = GPIO_Speed_100MHz;
	GPIO_Init(GPIOA, &GPIO_Initstructure);
}

void InitTimer()
{
	xButtonTimer = xTimerCreate((const char*)"Long Press timer", pdMS_TO_TICKS(LONG_PRESS_THRESHOLD), pdFALSE, ( void * ) 0, vLongPressEvent);
	xDoublePressTimer = xTimerCreate((const char*)"Double Press timer", pdMS_TO_TICKS(LONG_PRESS_THRESHOLD), pdFALSE, ( void * ) 0, vDoublePressEvent);
}

void turnOnLed(uint16_t led)
{
	GPIO_ResetBits(GPIOD, 0xFFFF);
	GPIO_SetBits(GPIOD, led);
}

//Code adapted from sample project.
void vPlaySound(void * params)
{
	float frequency;
	for (;;)
	{
		xQueueReceive(soundQueue, &frequency, portMAX_DELAY);
		codec_ctrl_init();
		I2S_Cmd(CODEC_I2S, ENABLE);
		for (sampleCounter = 0; sampleCounter < SOUND_DURATION * 4000; sampleCounter++)
		{
			if (SPI_I2S_GetFlagStatus(CODEC_I2S, SPI_I2S_FLAG_TXE))
			{
				SPI_I2S_SendData(CODEC_I2S, sample);
				//only update on every second sample to insure that L & R ch. have the same sample value
				if (sampleCounter & 0x00000001)
				{
					sawWave += frequency;
					if (sawWave > 1.0)
						sawWave -= 2.0;
					filteredSaw = updateFilter(&filt, sawWave);
					sample = (int16_t)(NOTEAMPLITUDE*filteredSaw);
				}
				sampleCounter++;
			}
		}
		codec_ctrl_init();
	}
}

void playSound(Pizza * pizza)
{
	xQueueSendToBack(soundQueue, (void *) &(pizza->freq), 0);
}

boolean hasMissedDeadline(Pizza * pizza)
{
	return timeElapsed % pizza->period >= pizza->deadline;
}

boolean finishedCooking(Pizza * pizza)
{
	return pizza->wcet == pizza->timeCooked;
}

PizzaType maxPriority(PizzaType type1, PizzaType type2)
{
	Pizza * pizza1 = &pizzas[type1];
	Pizza * pizza2 = &pizzas[type2];
	return pizza1->priority < pizza2->priority? type2 : type1;
}

PizzaType fixedPriority(PizzaType pizzaType1, PizzaType pizzaType2)
{
	Pizza * pizza1 = &pizzas[pizzaType1];
	Pizza * pizza2 = &pizzas[pizzaType2];
	PizzaType higherPriority;
	
	if (hasMissedDeadline(pizza1) == hasMissedDeadline(pizza2))
		higherPriority = maxPriority(pizzaType1, pizzaType2);
	
	else 
		higherPriority = hasMissedDeadline(pizza2)? pizzaType1 : pizzaType2;
	
	return higherPriority;
}

int timeUntilDeadline(Pizza * pizza)
{
	return pizza->deadline - timeElapsed % pizza->period;
}

PizzaType leastLaxityFirst(PizzaType pizzaType1, PizzaType pizzaType2)
{
	Pizza * pizza1 = &pizzas[pizzaType1];
	Pizza * pizza2 = &pizzas[pizzaType2];
	int timeLeft1 = timeUntilDeadline(pizza1) - (pizza1->wcet - pizza1->timeCooked);
	int timeLeft2 = timeUntilDeadline(pizza2) - (pizza2->wcet - pizza2->timeCooked);
	PizzaType higherPriority;
	
	if ((timeLeft1 < 0 && timeLeft2 < 0) || (timeLeft1 == timeLeft2))
		higherPriority = maxPriority(pizzaType1, pizzaType2);

	else if (timeLeft2 < 0 || timeLeft1 < 0)
		higherPriority = timeLeft2 < 0 ? pizzaType1 : pizzaType2;

	else
		higherPriority = timeLeft2 < timeLeft1 ? pizzaType2 : pizzaType1;;
	
	return higherPriority;
}

PizzaType earliestDeadlineFirst(PizzaType pizzaType1, PizzaType pizzaType2)
{
	int timeLeft1 = timeUntilDeadline(&pizzas[pizzaType1]);
	int timeLeft2 = timeUntilDeadline(&pizzas[pizzaType2]);
	PizzaType higherPriority;
	
	if ((timeLeft1 <= 0 && timeLeft2 <= 0) || (timeLeft1 == timeLeft2))
		higherPriority = maxPriority(pizzaType1, pizzaType2);

	else if (timeLeft2 <= 0 || timeLeft1 <= 0)
		higherPriority = timeLeft2 <= 0 ? pizzaType1 : pizzaType2;

	else
		higherPriority = timeLeft2 < timeLeft1 ? pizzaType2 : pizzaType1;;
	
	return higherPriority;
}


PizzaType chooseHigherPriority(PizzaType pizzaType1, PizzaType pizzaType2)
{
	Pizza * pizza1 = &pizzas[pizzaType1];
	Pizza * pizza2 = &pizzas[pizzaType2];
	
	PizzaType higherPriority;
	
	if (finishedCooking(pizza2))
		higherPriority = pizzaType1;
	
	else if (finishedCooking(pizza1))
		higherPriority = pizzaType2;

	else
		higherPriority = schedulingDisciplineCompare(pizzaType1, pizzaType2);
	
	return higherPriority;
}

void vMissedDeadlineAlert(void * params)
{
	for (;;)
	{
		xSemaphoreTake(missedDeadlineSemaphore, portMAX_DELAY);
		uint16_t leds = 0;
		Pizza * pizza;
		for (int i = 0; i < NUM_PIZZAS; i++)
		{
			pizza = &pizzas[i];
			if (timeUntilDeadline(pizza) == 0 && !finishedCooking(pizza))
			{
				leds |= pizzas[i].led;
				missedDeadlines[i]++;
			}
		}
		vTaskDelay(100);
		GPIO_ToggleBits(GPIOD, leds);
		vTaskDelay(50);
		GPIO_ToggleBits(GPIOD, leds);
		vTaskDelay(50);
		GPIO_ToggleBits(GPIOD, leds);
		vTaskDelay(100);
		GPIO_ToggleBits(GPIOD, leds);
		timeElapsed += 1;
	}
}

void setAlgorithm(int newAlgorithm)
{
	uint16_t leds[] = {
		GPIO_Pin_13,
		GPIO_Pin_14,
		GPIO_Pin_15,
	};
	selectedAlgorithm = newAlgorithm;
	schedulingDisciplineCompare = schedulingAlgorithms[newAlgorithm];
	turnOnLed(leds[newAlgorithm]);
}

void cycleSchedulerAlgorithm()
{
	setAlgorithm((selectedAlgorithm +1) % NUM_SCHED_ALGORITHMS);
}

void startScheduler()
{
	timeElapsed = 0;
	for (int i = 0; i < NUM_PIZZAS; i++)
	{
		missedDeadlines[i] = 0;
		pizzas[i].timeCooked = 0;
	}
	scheduledPizza = NUM_PIZZAS - 1;
}

void scheduler()
{
	boolean taskStarted = false;
	int h, v, p, s;
	TaskData * nextTask;
	PizzaType currHighest;
	PizzaType nextIndex;

	while (!taskStarted && isCooking)
	{
		currHighest = (scheduledPizza + 1) % NUM_PIZZAS;
		
		if (timeElapsed == 50 * 40)
		{
			//This is a kind of ugly way of examining the 
			//number of missed deadlines. Set a breakpoint
			//and look at the values assigned to these variables.
			h = missedDeadlines[HAWAIIAN];
			v = missedDeadlines[VEGGIE];
			p = missedDeadlines[PEPPERONI];
			s = missedDeadlines[SEAFOOD];
		}
		
		for (int i = 0; i < NUM_PIZZAS; i++)
		{
			if (timeElapsed % pizzas[i].period == 0)
				pizzas[i].timeCooked = 0;
		}

		for (int i = 1; i < NUM_PIZZAS; i++)
		{
			nextIndex = (scheduledPizza + i + 1) % NUM_PIZZAS;
			currHighest = chooseHigherPriority(currHighest, nextIndex);
		}
		nextTask = &pizzaTasks[currHighest];
	
		if (finishedCooking(&pizzas[currHighest]))
		{
			vTaskDelay(1000);
			timeElapsed++;
		}
		else
		{
			scheduledPizza = currHighest;
			xSemaphoreGive(nextTask->semaphore);
			xSemaphoreGive(missedDeadlineSemaphore);
			taskStarted = true;
		}
	}
}

void startCooking()
{
	startScheduler();
	scheduler();
}

void stopCooking()
{
	isCooking = false;
	selectedAlgorithm = -1;
}

void shortPressEvent()
{
	cycleSchedulerAlgorithm();
}

void longPressEvent()
{
	if (selectedAlgorithm >= 0)
	{
		isCooking = true;
		GPIO_ResetBits(GPIOD, 0xFFFF);
		startCooking();
		xTimerStop(xButtonTimer, 0);
	}
}

void doublePressEvent() {
	isCooking = false;
	stopCooking();
	xTimerStop(xDoublePressTimer, 0);
}

void vLongPressEvent(void *pvParameters) {
	
}

void vDoublePressEvent (void *pvParameters) {
	isDoublePress = false;
}

void detectDoublePress()
{
	while (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) == 0);
	vTaskDelay(BOUNCE_THRESHOLD / portTICK_RATE_MS);
	if (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) > 0)
	{
		if (isDoublePress)
		{
			doublePressEvent();
			xTimerStop(xDoublePressTimer, 0);
		}
		
		while (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) > 0);
		vTaskDelay(BOUNCE_THRESHOLD / portTICK_RATE_MS);
		if (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) == 0)
		{
			if (!isDoublePress)
			{
				isDoublePress = true;
				xTimerReset(xDoublePressTimer, 0);
			}
			else
				isDoublePress = false;
		}
	}
}

void detectShortOrLongPress()
{
	while (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) == 0);
	vTaskDelay(BOUNCE_THRESHOLD / portTICK_RATE_MS);
	if (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) > 0)
	{
		xTimerReset(xButtonTimer, 0);
		while (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) > 0);
		vTaskDelay(BOUNCE_THRESHOLD / portTICK_RATE_MS);
		if (xTimerIsTimerActive(xButtonTimer))
		{
			shortPressEvent();
			xTimerStop(xButtonTimer, 0);
		}
		else
		{
			longPressEvent();
		}
	}
}

void detectButtonPress(void * pvParameters)
{
	for (;;)
	{
		if (isCooking)
		{
			detectDoublePress();
		}
		
		else
		{
			detectShortOrLongPress();
		}
	}
}

void pizzaTask(void * pvParameter)
{
	TaskData * taskData = (TaskData*) pvParameter;
	Pizza * pizza = taskData->pizza;
	for (;;)
	{
		if  (xSemaphoreTake(taskData->semaphore, portMAX_DELAY))
		{
			GPIO_SetBits(GPIOD, pizza->led);
			vTaskDelay(500 / portTICK_RATE_MS);
			GPIO_ResetBits(GPIOD, pizza->led);
			pizza->timeCooked++;
			if (pizza->timeCooked == pizza->wcet)
			{
				playSound(pizza);
				GPIO_ResetBits(GPIOD, pizza->led);
			}
			vTaskDelay(500 / portTICK_RATE_MS);
			scheduler();
		}
	}
}

void createTasks()
{
	TaskData * taskData;
	for (int i = 0; i < NUM_PIZZAS; i++)
	{
		taskData = &pizzaTasks[i];
		taskData->pizza = &pizzas[i];
		taskData->taskHandle = (TaskHandle_t)i;
		taskData->semaphore = xSemaphoreCreateBinary();
		xTaskCreate(pizzaTask, taskData->taskName, STACK_SIZE_MIN, (void*)taskData, 2, taskData->taskHandle);
	}
	
	xTaskCreate(detectButtonPress, "button task", STACK_SIZE_MIN, NULL, 1, NULL);
	xTaskCreate(vMissedDeadlineAlert, "deadline alert", STACK_SIZE_MIN, NULL, 2, NULL);
	xTaskCreate(vPlaySound, "sound task", STACK_SIZE_MIN, NULL, 2, NULL);
	missedDeadlineSemaphore = xSemaphoreCreateBinary();
}

int main()
{
	SystemInit();
	InitLeds();
	InitButton();
	InitTimer();
	soundQueue = xQueueCreate(4, sizeof(float));
	NVIC_PriorityGroupConfig( NVIC_PriorityGroup_4 );
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
	codec_init();
	initFilter(&filt);
	createTasks();
	vTaskStartScheduler();
}

// a very crude FIR lowpass filter
float updateFilter(fir_8* filt, float val)
{
	uint16_t valIndex;
	uint16_t paramIndex;
	float outval = 0.0;

	valIndex = filt->currIndex;
	filt->tabs[valIndex] = val;

	for (paramIndex=0; paramIndex<8; paramIndex++)
	{
		outval += (filt->params[paramIndex]) * (filt->tabs[(valIndex+paramIndex)&0x07]);
	}

	valIndex++;
	valIndex &= 0x07;

	filt->currIndex = valIndex;
	return outval;
}

void initFilter(fir_8* theFilter)
{
	uint8_t i;

	theFilter->currIndex = 0;

	for (i=0; i<8; i++)
		theFilter->tabs[i] = 0.0;

	theFilter->params[0] = 0.01;
	theFilter->params[1] = 0.05;
	theFilter->params[2] = 0.12;
	theFilter->params[3] = 0.32;
	theFilter->params[4] = 0.32;
	theFilter->params[5] = 0.12;
	theFilter->params[6] = 0.05;
	theFilter->params[7] = 0.01;
}
