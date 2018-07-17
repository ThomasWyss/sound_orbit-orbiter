

#include "fcntl.h"
#include <chrono>
#include <queue>
#include <mutex>
#include <csignal>
#include <stdio.h>
#include <thread>
#include <iostream>
#include <sstream>
#include <string.h>
#include "wiringPi.h"
#include "cmaxonmotor.h"
#include "fmod.hpp"
#include "TCPClient.h"
#include "Toolbox.h"
#include "StimuliLibrary.h" 
#include "Movement.h"
#include "tcpParameterRequestHandler.h"
using namespace std;

#define WHEELPERI float(0.03 * 3.1415) // Antriebsrad (Durchmesser[m] * Pi) .30mm
#define RAILPERI  float(2.048 * 3.1415)   // Kreisumfnag (Durchmesser[m] * Pi)

unsigned short iVoltage;
long lMotorMovementToProcess;
int iCurrentPosition, iNumbOffs, iAngle;
unsigned char cErrorNbr, cNumb[3]; // Motor errors
std::shared_ptr<CMaxonMotor>  motor;
std::shared_ptr<tcpParameterRequestHandler> pTCPParameterRequestHandler;
std::shared_ptr<Movement> pMovement;
bool exit_app;
std::queue<std::string> tcp_queue;
std::mutex tcp_mutex;
TCPClient tcp;
char *ip_addr;
int port;

StimuliLibrary stimuliLib;


void tcp_func() {
	std::string msg;

	while (!exit_app) {
		msg = tcp.receive(100);
		if (msg.length() == 0) {
			tcp.exit();
			std::cout << "Reconnecting..." << std::endl;

			tcp.setup(ip_addr, port);
			sleep(2);
		}
		tcp_mutex.lock();
		tcp_queue.push(msg);
		tcp_mutex.unlock();
	}
}

// LED Pin - wiringPi pin 0 is BCM_GPIO 17.
// we have to use BCM numbering when initializing with wiringPiSetupSys
// when choosing a different pin number please use the BCM numbering, also
// update the Property Pages - Build Events - Remote Post-Build Event command 
// which uses gpio export for setup for wiringPiSetupSys
#define	LED	17
#define TaskTime 200


#define SOUNDS_DIR "/usr/lib/Orbiter/sounds"



int n_notes = 12;
std::string notes[] = { "A", "A#", "B", "C" , "C#", "D", "D#", "E", "F", "F#", "G", "G#" };
float pitches[] = { 1.0f,
1.0594630943592953f,
1.122462048309373f,
1.189207115002721f,
1.2599210498948732f,
1.3348398541700344f,
1.4142135623730951f,
1.4983070768766815f,
1.5874010519681994f,
1.681792830507429f,
1.7817974362806785f,
1.8877486253633868f };

float A_freqs[] = { 22.5f, 55.0f,110.0f,220.0f,440.0f,880.0f,1760.0f,3520.0f,7040.0f };



void calcMovementToProcess(void) {
	lMotorMovementToProcess = 65536 * (float(iAngle) / 360.0) * (RAILPERI / WHEELPERI);
}



void IdleFunc(void) {
	motor->getCurrentPosition(iCurrentPosition);
	motor->ErrorNbr(&cErrorNbr);
	if (motor->ErrorCode == 0x34000007) motor->initializeDevice();
	if (cErrorNbr != 0) motor->initializeDevice();
}

bool movement_skip;
bool stimuli_skip;
void vProcessMovement()
{
	shared_ptr<Toolbox::HostData> hostData = pMovement->movement_queue.front();
	pMovement->movement_queue.pop();

	std::cout << "vProcessMovement:" << std::endl;
	std::cout << "hostData: " << "dir = " << static_cast<int>(hostData->direction) << ", angleToMove = " << hostData->angularDistance << ", speed = " << hostData->speed << std::endl;
	std::cout << "stim_nr = " << static_cast<int>(hostData->stimulus_nr) << ", stim_dur = " << hostData->stimulusDuration << ", vol = " << hostData->loudness << ", toBeTriggerd = " << hostData->toBeTriggerd << std::endl;

	//DEBUG! Bei speaker 01 ist das kabel nicht gleich wie bei den anderen im controller. vermutlich muss hier mit der EPSON software neu konfiguriert werden?
	if (RaspiConfig::ownIndex == 1)
	{
		if (hostData->direction == 1) { // Dir 1 = clockwise
			iAngle = hostData->angularDistance; // Correct
		}
		if (hostData->direction == 2) { // Dir 2 = counterclockwise
			iAngle = hostData->angularDistance * -1;
		}
	}
	else
	{
		if (hostData->direction == 1) { // Dir 1 = clockwise
			iAngle = hostData->angularDistance * -1; // Correct
		}
		if (hostData->direction == 2) { // Dir 2 = counterclockwise
			iAngle = hostData->angularDistance;
		}
	}

	if (hostData->direction != 0) { // Dir 0 = no movement
		calcMovementToProcess();
		motor->setSpeed(hostData->speed);
		motor->Move(lMotorMovementToProcess);
	}
}
void TimerFunc(bool& bIsFirstCall) {

	std::string host_data_raw;
	while (!tcp_queue.empty()) {
		tcp_mutex.lock();
		host_data_raw = tcp_queue.front(); // Get tcp messages
		tcp_queue.pop();
		tcp_mutex.unlock();
	}

	/* PROTOCOL INTERPRETATION */
	int speakerIDX;
	if (host_data_raw.length() != 0) 
	{ // If a tcp-message has arrived
		std::cout << "\n Raw hostData input: " << host_data_raw << std::endl;

		// FIRST: Check if it is a get or set request for raspi data
		char charIsGetOrSetRequest = host_data_raw.at(0);
		//std::cout << "charIsGetOrSetRequest " << charIsGetOrSetRequest << std::endl;
		if (charIsGetOrSetRequest == 'G')
		{
			std::string strsAnsnwerToServerRequest;
			strsAnsnwerToServerRequest = pTCPParameterRequestHandler->interpretRequest(host_data_raw);

			tcp.Send(strsAnsnwerToServerRequest);
			std::cout << "Battery Voltage" << strsAnsnwerToServerRequest <<endl;;
		}
		else if (charIsGetOrSetRequest == 'S') // no answer sned needed
		{
			std::string strsAnsnwerToServerRequest;
			strsAnsnwerToServerRequest = pTCPParameterRequestHandler->interpretRequest(host_data_raw);
		}
		else
		{
			shared_ptr<Toolbox::HostData> hostData(new Toolbox::HostData(Toolbox::decodeHostData(host_data_raw))); // decode host data
			if (hostData->mov_queued) { // Add new data to queue
				std::cout << "Add new data to queue" << endl;
				pMovement->movement_queue.push(hostData);
				movement_skip = false;
			}
			else { // Clear Queue and Add new Data to Queue
				std::cout << " Clear Queue and Add new Data to Queue" << endl;
				movement_skip = true;
				while (!pMovement->movement_queue.empty())
				{
					pMovement->movement_queue.pop();
				}
				pMovement->movement_queue.push(hostData);
			}
			
			if (hostData->stim_queued) { // Add new data to queue
				stimuliLib.stimuli_queue.push(hostData);
				stimuli_skip = false;
			}
			else { // Clear Queue and Add new Data to Queue
				stimuli_skip = true;
				while (!stimuliLib.stimuli_queue.empty())
				{
					stimuliLib.stimuli_queue.pop();
				}
				stimuliLib.stimuli_queue.push(hostData);
			}
		}

	}

	/* STIMULI PROCESSING */
	
	stimuliLib.updateFSystem();
	// Check if there is a protocol hicjacking
	if (!stimuliLib.bAdaptStimulusParametersDueToHijacking(pMovement->movement_queue,motor)) // not protocl adaption, process as usual
	{
		//cout << "No Hijacking" << endl;
		if (stimuliLib.bGetIsThereAFractionLeftToPlay())
		{
			//printf("\n We have a fraction left to play of %d milliseconds\n", stimuliLib.uiGetDesiredStimuliDuration_ms());
			stimuliLib.playStimuli(); // Enter here only if (audioFileLength_ms < desiredDuration_ms)
		}
		else if (!stimuliLib.stimuli_queue.empty())
		{
			//cout << "We try to play a stimulus if it has to be triggered" << endl;
			stimuliLib.vPlayStimulusIfToBeTriggered();
		}
	}	
}

void vMovementThread(bool &bIsFirstCall)
{
	std::thread movementThread{ [&]()
	{
		while (true)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			/* MOVEMENT PROCESSING */
			if (!pMovement->movement_queue.empty())// movement pending 
			{
				if (motor->reachedTarget() || movement_skip) // (movementFinnished OR Skip_this_movement)
				{
					vProcessMovement();
				}
				// Here we ware if we have some movements in our queue which we want to do but still other movements are going on
				else if (bIsFirstCall == true)
				{
					bIsFirstCall = false;
					cout << "Movement due to first call" << endl;
					vProcessMovement();
					// is Motor moving? else: process further movement
					//cout << "++++++++++++++++++++++++++Motor not in position we wait for movement to finish" << endl;
					//vProcessMovement();
				}
			}
		}
	}};
	movementThread.detach(); // Prevents the thread from bein destroyed when the its out of scope
}

int main(int argc, char **argv)
{
	pMovement = Movement::getInstance();
	printf("Starting Orbiter Program.");
	char InterfaceName[] = "USB0";
	motor = std::make_shared<CMaxonMotor>(InterfaceName, 1);
	motor->initializeDevice(); // initialize EPOS2
	pTCPParameterRequestHandler = std::make_shared<tcpParameterRequestHandler>(motor);

	std::thread tcp_thread(tcp_func);
	if (argc == 1)
	{
		printf("No Ip provided: use: 192.168.178.23\n");
		argv[1] = "192.168.178.23";
	}
	ip_addr = argv[1];
	port = 1234;
	do {
		printf("Connecting to server at %s:%d ... \n", ip_addr, port);
	} while (!tcp.setup(ip_addr, port));
	printf("Connected!");

	exit_app = false;
	bool bIsFirstCall = true;
	bool bRef = &bIsFirstCall;
	vMovementThread(bIsFirstCall);
	while (!exit_app) {
		TimerFunc(bRef);
		//IdleFunc(); -> this one takes very very much time!
		//if (!exit_app)
		//{
			//usleep(100000);
		//}
	}
	motor->closeDevice(); // close EPOS2
	printf("\n Delete motor object quit main!");

	return 0;
}