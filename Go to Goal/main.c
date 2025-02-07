/*
 * diffdrive128.c
 *
 * Created: 9/11/2016 2:58:40 PM
 *  Author: Drishti

 */ 


#define F_CPU 8000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sfr_defs.h>
#include "USART_128.h"
#include "compass_sensor.h"
#include "movingArray.h"
#include <math.h>

// software var
#define PIDLoop_mainLoop_ratio		1
#define pi							3.1416
#define rpmMovArrayLength			10
#define	timeInterval				0.03264			// 1024 * 255 / F_CPU // in sec
#define leftPWM						OCR1A
#define rightPWM					OCR1B

// hardware var
#define ticksPerRotation			1000
#define r							5.0
#define L							22.2
#define circumference				31.4				//2 * pi * r
#define vmax					    75

//PD2 and PD3 RX and TX
//PD0 and PD1 SCL and SDA
// PE4 and PE6 interrupt pins
//PE3 and PE5 GPIO pins for encoder
// PB5 and PB6 PWM pins
//dir1-PG3 dir2-PB7

struct position {float x; float y; int phi;};
struct unicycleState {float v; float w;};
struct differentialState {float leftRPM; float rightRPM;};


enum {left, right}; //wheel
enum {lRPM, rRPM, angularVel}; // movingArray, PID

volatile long ticks[2] = {0,0};
volatile long tick1[2] = {0,0};
volatile struct position curBotPosition;
volatile struct position desiredBotPosition;
volatile struct differentialState curDiffState;
volatile struct differentialState curDiffState1;
volatile struct differentialState desiredDiffState;
int unitTimeCount = 0;
volatile float phi_ref = 0;
volatile float kp[3] = {0.26,0.26,0.31}, ki[3] = {0.0, 0.0, 0.0}, kd[3] = {1.68, 1.65, 0.0002}, E[3] = {0, 0, 0}, e_old[3] = {0, 0, 0};        
//volatile float kp[3] = {4,4,0}, ki[3] = {0.0, 0.0, 0.0}, kd[3] = {4, 1.53, }, E[3] = {0, 0, 0}, e_old[3] = {0, 0, 0};
	

int timekeeper = 0;
float ang=0;
int8_t flag;
int32_t data;


float PID(float error,int x) 
{
	float pid = 0;
	pid = (kp[x]*error) + (ki[x]*E[x]) + (kd[x]*(error - e_old[x]));
	E[x]+=error;
	e_old[x] = error;
	return pid;
}

inline void Graph_Plot()
{
	USART_Transmitchar(0xAB,0);
	USART_Transmitchar(0xCD,0);
	USART_Transmitchar(0x08,0);
	USART_Transmitchar(0x00,0);
// 	USART_Transmitchar((int)curDiffState.leftRPM & 0x00FF,0);
// 	USART_Transmitchar((((int)curDiffState.leftRPM & 0xFF00) >> 8),0);
// 	USART_Transmitchar((int)curDiffState.rightRPM & 0x00FF,0);
// 	USART_Transmitchar((((int)curDiffState.rightRPM & 0xFF00) >> 8),0);
// 	USART_Transmitchar((int)desiredDiffState.leftRPM & 0x00FF,0);
// 	USART_Transmitchar((((int)desiredDiffState.leftRPM & 0xFF00) >> 8),0);
// 	USART_Transmitchar((int)desiredDiffState.rightRPM & 0x00FF,0);
// 	USART_Transmitchar((((int)desiredDiffState.rightRPM & 0xFF00) >> 8),0);
	USART_Transmitchar((int)desiredBotPosition.phi & 0x00FF,0);
	USART_Transmitchar((((int)desiredBotPosition.phi & 0xFF00) >> 8),0);
	USART_Transmitchar((int)curBotPosition.phi & 0x00FF,0);
	USART_Transmitchar((((int)curBotPosition.phi & 0xFF00) >> 8),0);
	
}

float degreeToRad(float degree) {
	return degree * pi / 180.0;
}

float radToDegree(float rad) {
	return rad * 180.0 / pi;
}

float normalizeAngle(float degree) {
	return radToDegree(atan2(sin(degreeToRad(degree)), cos(degreeToRad(degree))));
}

float sigmoid(int z) 
{
	return tanh(z/30);
}

struct unicycleState getDesiredUnicycleState(struct position curBotPosition, struct position desiredBotPosition) {
	struct unicycleState desiredState;
	
	int errDist = sqrt((desiredBotPosition.x - curBotPosition.x) * (desiredBotPosition.x -curBotPosition.x) + (desiredBotPosition.y - curBotPosition.y)*(desiredBotPosition.y - curBotPosition.y));
	int desiredPhi = 0;
		if((curBotPosition.x - desiredBotPosition.x) == 0) 
		{	if (desiredBotPosition.y - curBotPosition.y ==0 )
			{desiredPhi = 0;
			}
			else if(desiredBotPosition.y > curBotPosition.y) 
			{desiredPhi = 90;
			} 
			else 
			{desiredPhi = -90;
			}
		} 
		else if((curBotPosition.y - desiredBotPosition.y) == 0) 
		{	if(desiredBotPosition.x > curBotPosition.x) 
			{	desiredPhi = 0;
			} 
			else 
			{desiredPhi = 180;
			}
		} 
		else 
		{desiredPhi = radToDegree(atan((desiredBotPosition.y - curBotPosition.y) / (desiredBotPosition.x - curBotPosition.x)));
		}
	//desiredPhi=0;
	desiredState.v = vmax * sigmoid(errDist);
	desiredState.w = PID(normalizeAngle(desiredPhi - curBotPosition.phi), angularVel); 
	return desiredState;
}

struct differentialState transformUniToDiff(struct unicycleState uniState)
 {
	struct differentialState diffState;
	//using the kinematics equations
	float vleft = (2*uniState.v -L*uniState.w) / (2 * r);
	float vright = (2*uniState.v + L*uniState.w)/(2 * r);
	diffState.rightRPM = vright / circumference * 60;
	diffState.leftRPM = vleft / circumference * 60;
	return diffState;
}


void calculateDiffState() {
	int x;
	int sampledTicks[] = {ticks[left], ticks[right]};
	
	ticks[0] = 0;
	ticks[1] = 0;
	
	for(x = 0; x <2 ; x++) {
		float rpm = sampledTicks[x] * 0.91911764;//	constant = 60 / ticksPerRotation / (timeInterval*2);
		
		addElement(rpm, x,0);
	}
	curDiffState.leftRPM = getAverage(lRPM,0);
	curDiffState.rightRPM = getAverage(rRPM,0);
}


void calculateDiffState1() {
	int x;
	int sampledTick1[] = {tick1[left], tick1[right]};
	tick1[0] = 0;
	tick1[1] = 0;
	
	for(x = 0; x <2 ; x++) {
		float rpm1 = sampledTick1[x] * 0.91911764;    //	constant = 60 / ticksPerRotation / (timeInterval*2);
		
		addElement(rpm1,x,2);
		
	}
	curDiffState1.leftRPM = getAverage(lRPM,2);
    curDiffState1.rightRPM = getAverage(rRPM,2);
	
}

void calculatePos() {
	curBotPosition.phi = normalizeAngle(phi_ref - getHeading());
	float leftDist = curDiffState.leftRPM * timeInterval / 60.0 * circumference;
	float rightDist = curDiffState.rightRPM * timeInterval / 60.0 * circumference;
	
	float dist = (leftDist + rightDist) / 2;
	
	curBotPosition.x += dist * cos(degreeToRad(curBotPosition.phi)); 
	curBotPosition.y += dist * sin(degreeToRad(curBotPosition.phi));
}

		
void changeWheelOutputs(struct differentialState curState, struct differentialState desiredState) {
	float leftPID = leftPWM;
	float rightPID = rightPWM;
	
	if(desiredState.leftRPM<0)
	{
		desiredState.leftRPM=(-1)*desiredState.leftRPM;
		PORTG|=(1<<PING3);
	}
	else
	{
		PORTG&=~(1<<PING3);
	}
	
	
	if(desiredState.rightRPM<0)
	{
		desiredState.rightRPM=(-1)*desiredState.rightRPM;
		PORTB|=(1<<PINB7);
	}
	else
	{
		PORTB&=~(1<<PINB7);
	}
	
	leftPID += PID(desiredState.leftRPM - curState.leftRPM, left);
	rightPID += PID(desiredState.rightRPM - curState.rightRPM, right);
	

	if(leftPID > 1023) {
		leftPID = 1023;
		} else if(leftPID < 0){
		leftPID = 0;
	}
	if(rightPID > 1023) {
		rightPID = 1023;
		} else if(rightPID < 0){
		rightPID = 0;
	}
	
	leftPWM = lround(leftPID);
	rightPWM = lround(rightPID);
	
}

int main() {
	_delay_ms(100); // time to let compass sensor load
	 
	DDRB |= (1<<PB5) | (1<<PB6) | (1<<PB7) ;
	DDRG |= (1<<PG3);
	
	//interrupt , any logical change
	EICRA |= (1<<ISC20) | (1<<ISC21)| (1<<ISC30) | (1<<ISC31);
	EIMSK |= (1<<INT2) | (1<<INT3);
	
	//timers 10 bit
	TCCR0 |= (1<<CS02) | (1<<CS01) | (1<<CS00);
	TIMSK |= (1<<TOIE0);
	
	//PWM_timer , Fast_PWM_mode, Top = 0x03FF(in Hex) or 1023(in Decimal)
	TCCR1B |= (1<<CS10) | (1<<WGM12);
	TCCR1A |= (1<<COM1A1) | (1<<COM1B1) | (1 << WGM11) | (1<< WGM10);
	
	init_HMC5883L();
	
	USART_Init(51,0);
	USART_InterruptEnable(0);
	sei();
	
	phi_ref = getHeading();				//initializing the first ever angle taken by the compass sensor as reference
	
	init_movingArray(rpmMovArrayLength, lRPM);
	init_movingArray(rpmMovArrayLength, rRPM);
	
	//taking initial point as origin
	curBotPosition.x = 0;
	curBotPosition.y = 0;
	
	desiredBotPosition.x =100;
	desiredBotPosition.y =0;

// 	desiredDiffState.leftRPM=10;
// 	desiredDiffState.rightRPM=10;
	
	while (1) 
	{	
		
	}
}

ISR(TIMER0_OVF_vect) {
	
	unitTimeCount++;
	
	if(timekeeper == 2)
	 { 
		calculateDiffState1();
		calculateDiffState();
		timekeeper = 0;
	}	timekeeper++;
	
	calculatePos();
	
	if(unitTimeCount == PIDLoop_mainLoop_ratio)
	{	
		desiredDiffState = transformUniToDiff(getDesiredUnicycleState(curBotPosition, desiredBotPosition));
		unitTimeCount = 0;
	}
	changeWheelOutputs(curDiffState1, desiredDiffState);
	
	USART_Transmitchar('l',0);
    USART_TransmitNumber(curDiffState1.leftRPM,0);
	USART_Transmitchar(0x0A,0);
	USART_Transmitchar('r',0);
	USART_TransmitNumber(curDiffState1.rightRPM,0);
	USART_Transmitchar(0x0A,0);
	USART_Transmitchar('x',0);
	USART_TransmitNumber(ticks[left],0);
	USART_Transmitchar(0x0A,0);
	USART_Transmitchar('y',0);
	USART_TransmitNumber(ticks[right],0);
	USART_Transmitchar(0x0A,0);
	USART_Transmitchar('p',0);
	USART_TransmitNumber(curBotPosition.phi,0);
	USART_Transmitchar(0x0A,0);
	USART_Transmitchar('d',0);
	USART_TransmitNumber(desiredBotPosition.x,0);
	USART_Transmitchar(0x0D,0);
	//Graph_Plot();
}

ISR(INT3_vect) {
	if(bit_is_clear(PINE,3))
	ticks[left]++;
	else if(bit_is_set(PINE,3))
	{ticks[left]--;} 

	tick1[left]++;
}

ISR(INT2_vect) {
	if(bit_is_clear(PINB,0))
	ticks[right]++;
	else if(bit_is_set(PINB,0))
	{ticks[right]--;}

	tick1[right]++;
}

ISR(USART0_RX_vect)
{
	char m;
	
	m=USART_Receive(0);
	
	if (m=='a')
	{
		desiredBotPosition.x=data;
		data=0;
	}
	else
	{
		data=data*10+(m-'0');
		
	}
	
}

