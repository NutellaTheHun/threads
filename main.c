#include <stdlib.h>
#include <sys/time.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define CUSTOMERCOUNT 1000

typedef struct customer customer;
struct customer {
	struct timeval arrivalTime;
	customer* next;
};

typedef struct thread1_data thread1_data;
struct thread1_data {
	double* customerRate;
	double* mean;
	double* X2;
	int* count;
};

typedef struct thread2_data thread2_data;
struct thread2_data {
	double* customerRate;
	double* sMean;
	double* sX2;
	int* sCount;

	double* wMean;
	double* wX2;
	int* wCount;

	double* serverProcessTime;
};

typedef struct thread3_data thread3_data;
struct thread3_data {
	double* qMean;
	double* qX2;
	int* qCount;
};

void* thread1(void* p);
void* thread2(void* p);
void* thread3(void* arg);
void* threadXYZ(void* p);
double drawRandExpDist(double lambda);
double rndExp(struct drand48_data* randData, double lambda);
int srand48_r(long int seedval, struct drand48_data* buffer);
int drand48_r(struct drand48_data* restrict buffer, double* restrict result);

void onlineAvg(double* avg, double* sumX2, double input, int size);
void offlineStd(double* avg, double* sumX2, double* std, int size);

void printStatistics(double mean_interArrival, double std_interArrival, double mean_waiting, double std_waiting,
	double mean_serviceTime, double std_serviceTime, double mean_queueLen, double std_queueLen, double serverUtilization);
/*
The variables qHead, qTail, qLength, and qMutex are global variables shared by all threads.Thread 1 inserts a
new customer at the tail.Thread 2 removes a customer from the head.
*/

customer* qHead = NULL; //thread 2 takes from head
customer* qTail = NULL; //thread 1 inserts at tail
unsigned qLength; //total size of linked list queue (qHead, qTail)
pthread_mutex_t qMutex; //mutex for control over linked list queue
pthread_mutex_t t2Mutex; //linked list for service threads(thread 2) control over calculating online average of waiting and service times, and incrementing their count
pthread_mutex_t t2SMutex; //controls accessing serverBusyTime accross server threads (thread 2)
pthread_cond_t queueEmpty; //thread 1 signals on queueEmpty when queue changes from empty -> nonempty, thread 2 waits on condition when queue is empty.
int serviceThreadDone; //control variable to signal thread 1 and 3 when 2s are finished.
int customerCount; //thread 2 increments this variable when a customer service is completed, global for when there're multiple thread 2s

int main(int argc, char* argv[]) {

	//statistic variables
	double mean_interArrivalTime = 0.0;
	double interArrivalTimeX2 = 0.0;
	double std_interArrivalTime = 0.0;
	int interArrivalTimeCount = 0;

	double mean_waitingTime = 0.0;
	double waitingTimeX2 = 0.0;
	double std_waitingTime = 0.0;
	int waitingTimeCount = 0;

	double mean_serviceTime = 0.0;
	double serviceTimeX2 = 0.0;
	double std_serviceTime = 0.0;
	int serviceTimeCount = 0;

	double mean_queueLength = 0.0;
	double queueLengthX2 = 0.0;
	double std_queueLength = 0.0;
	int queueLengthCount = 0;

	struct timeval simulationStartTime;
	struct timeval simulationEndTime;
	double totalServerTime;
	double serverBusyTime = 0.0;
	double serverUtilization;

	//command line flags
	// -l
	double arrivalRate = 5.0;

	//-m
	double serviceRate = 7.0;

	//-c
	int totalCustomers = 10;

	//-s
	int totalServers = 1;

	//structs to pass statistics for online calculations and arrival/service rates
	thread1_data thrd1_data;
	thrd1_data.customerRate = &arrivalRate;

	thrd1_data.mean = &mean_interArrivalTime;
	thrd1_data.X2 = &interArrivalTimeX2;
	thrd1_data.count = &interArrivalTimeCount;

	thread2_data thrd2_data;
	thrd2_data.customerRate = &serviceRate;

	thrd2_data.sMean = &mean_serviceTime;
	thrd2_data.sX2 = &serviceTimeX2;
	thrd2_data.sCount = &serviceTimeCount;

	thrd2_data.wMean = &mean_waitingTime;
	thrd2_data.wX2 = &waitingTimeX2;
	thrd2_data.wCount = &waitingTimeCount;

	thrd2_data.serverProcessTime = &serverBusyTime;

	thread3_data thrd3_data;
	thrd3_data.qMean = &mean_queueLength;
	thrd3_data.qX2 = &queueLengthX2;
	thrd3_data.qCount = &queueLengthCount;

	//getOpt reference https://www.geeksforgeeks.org/getopt-function-in-c-to-parse-command-line-arguments/
	int opt = 0;

	//command line parsing
	while ((opt = getopt(argc, argv, "l:m:c:s:")) != -1){
		switch (opt)
		{
		case 'l': //arrival rate
			arrivalRate = atof(optarg);
			break;
		case 'm': // service rate
			serviceRate = atof(optarg);
			break;
		case 'c': // numCustomers
			totalCustomers = atoi(optarg);
			break;
		case 's': //numServer
			totalServers = atoi(optarg);
			break;
		case '?': //used for unknown options
			printf("unknown option: %c\n", optopt);
			break;

		}
	}
	
	//1 for customer generator thread (thread 1), and 1 for queue length observer (thread 3)
	int totalThreads = totalServers + 2;

	pthread_t th[totalThreads]; 
	//int result;

	//mutex and condition initializations
	pthread_mutex_init(&qMutex, NULL); 
	pthread_mutex_init(&t2Mutex, NULL);
	pthread_mutex_init(&t2SMutex, NULL);
	pthread_cond_init(&queueEmpty, NULL);
	serviceThreadDone = 0;

	customerCount = 0;
	
	gettimeofday(&simulationStartTime, NULL);
	//start generator thread 1
	if ((pthread_create(&th[0], NULL, thread1, (void*)&thrd1_data)) != 0) {
		perror("Thread1 creation failed");
		return -1;
	}
	
	//start server theads (2)
	for (int i = 1; i < totalServers + 1; i++) {
		if ((pthread_create(&th[i], NULL, thread2, (void*)&thrd2_data)) != 0) {
			perror("Thread2 creation failed");
			return -1;
		}
	}

	//start observer thread 3
	if ((pthread_create(&th[totalServers + 1], NULL, thread3, (void*)&thrd3_data)) != 0) {
		perror("Thread3 creation failed");
		return -1;
	}
	
	// Wait for the thread to finish
	for (int i = 0; i < totalThreads; i++){
		if (pthread_join(th[i], NULL)) {
			perror("Thread join failed");
			return -1;
		}
	}
	gettimeofday(&simulationEndTime, NULL);
	
	pthread_mutex_destroy(&qMutex); 
	pthread_mutex_destroy(&t2Mutex);
	pthread_mutex_destroy(&t2SMutex);
	pthread_cond_destroy(&queueEmpty);
	
	//calculate server utilization
	totalServerTime = simulationEndTime.tv_sec - simulationStartTime.tv_sec + 
		(simulationEndTime.tv_usec - simulationStartTime.tv_usec) / 1000000.0;

	serverUtilization = serverBusyTime / totalServerTime;

	/*
	printf("totalServerTime: %lf\n", totalServerTime);
	printf("serverBusyTime: %lf\n", serverBusyTime);
	printf("serverUtilization: %lf\n", serverUtilization);
	*/
	
	//calculate STD offline
	offlineStd(&mean_interArrivalTime, &interArrivalTimeX2, &std_interArrivalTime, interArrivalTimeCount);
	offlineStd(&mean_waitingTime, &waitingTimeX2, &std_waitingTime, waitingTimeCount);
	offlineStd(&mean_serviceTime, &serviceTimeX2, &std_serviceTime, serviceTimeCount);
	offlineStd(&mean_queueLength, &queueLengthX2, &std_queueLength, queueLengthCount);

	printStatistics(mean_interArrivalTime, std_interArrivalTime, mean_waitingTime, std_waitingTime, 
		mean_serviceTime, std_serviceTime, mean_queueLength, std_queueLength, serverUtilization);

	return 0;
}


double rndExp(struct drand48_data* randData, double lambda) {
	double tmp;
	drand48_r(randData, &tmp);
	return -log(1.0 - tmp) / lambda;

}

double drawRandExpDist(double lambda) {
	struct drand48_data randData;
	struct timeval tv;
	double result;

	gettimeofday(&tv, NULL);

	//to seed the generator 
	srand48_r(tv.tv_sec + tv.tv_usec, &randData);

	//to draw a number from [0, 1) uniformly and store it in "result" 
	drand48_r(&randData, &result);

	//will be arrivalTime or serviceTime;
	result = rndExp(&randData, lambda);

	//printf("Random number: %f\n", result);

	return result;
}

void* thread1(void* arrivalData) {
	//printf("Enter Thread1\n");
	customer* newCustomer;
	double aDrawTime;
	struct timespec sleepTime;
	struct timeval tv;

	thread1_data* thrd1_data = (thread1_data*)arrivalData;

	// Access the members of thrd1_data
	double* arrivalTime = thrd1_data->customerRate;
	double* mean_interArrivalTime = thrd1_data->mean;
	double* interArrivalTimeX2 = thrd1_data->X2;
	int* interArrivalTimeCount = thrd1_data->count;

	//loop
	while(serviceThreadDone == 0){
		//draw inter-arrival time from exponential distribution
		aDrawTime = drawRandExpDist(*(double*)arrivalTime);
		sleepTime.tv_sec = 0;
		sleepTime.tv_nsec = aDrawTime * 1000000000L;
		//printf("aDrawTime %lf\n", aDrawTime);
		
		//calculate inter-arrival time mean
		(*interArrivalTimeCount)++;
		onlineAvg(mean_interArrivalTime, interArrivalTimeX2, aDrawTime, *interArrivalTimeCount);

		//sleep for that much time
		nanosleep(&sleepTime, NULL);
		newCustomer = (customer*)malloc(sizeof(customer));

		//gettimeofday() timestamp the arrival time
		gettimeofday(&tv, NULL);

		newCustomer->arrivalTime.tv_sec = tv.tv_sec;
		//usec  1000000.0;
		newCustomer->arrivalTime.tv_usec = tv.tv_usec;

		//printf("customer tv_sec: %ld\n", newCustomer->arrivalTime.tv_sec);

		//lock qMutex
		pthread_mutex_lock(&qMutex);


		//consider 2 cases:
		
		//1. insert into a nonempty queue
		if (qHead != NULL) {
			qTail = qTail->next;
			qTail = newCustomer;
			
		}
		//2. insert into an empty queue -- signal thread 2
		else {
			qHead = newCustomer;
			qTail = qHead;
			pthread_cond_broadcast(&queueEmpty);
		}

		qLength++;
		//printf("qLength %d\n", qLength);

		//unlock qMutex
		pthread_mutex_unlock(&qMutex);

	}
}

//process CUSTOMERCOUNT customers, signal shutdown, pthread_cond serviceThreadDone
void* thread2(void* serviceData) {

	//printf("Enter Thread2\n");

	customer* aCustomer;
	struct timeval tv;
	double waitingTime;
	double sDrawTime;
	struct timespec sleepTime;
	struct timeval serviceStartTime;
	struct timeval serviceEndTime;
	long serviceTime;

	thread2_data* thrd2_data = (thread2_data*)serviceData;

	double* serviceRate = thrd2_data->customerRate;
	double* mean_serviceTime = thrd2_data->sMean;
	double* serviceTimeX2 = thrd2_data->sX2;
	int* serviceTimeCount = thrd2_data->sCount;

	double* mean_waitingTime = thrd2_data->wMean;
	double* waitingTimeX2 = thrd2_data->wX2;
	int* waitingTimeCount = thrd2_data->wCount;

	double* serverProcessTime = thrd2_data->serverProcessTime;

	while (customerCount < CUSTOMERCOUNT) {
		
		//lock qMutex
		pthread_mutex_lock(&qMutex);

		//consider 2 cases:
		//1. a nonempty queue: remove from head
		if (qHead != NULL) {
			gettimeofday(&serviceStartTime, NULL);
			aCustomer = qHead;

			if (qHead->next != NULL) {
				qHead = qHead->next;
			}
			else {
				qHead = NULL;
				qTail = NULL;
			}

			qLength--;

			pthread_mutex_unlock(&qMutex);

			gettimeofday(&tv, NULL); //timestamp the departure time

			waitingTime = tv.tv_sec - aCustomer->arrivalTime.tv_sec +
				(tv.tv_usec - aCustomer->arrivalTime.tv_usec) / 1000000.0;
			//printf("waitingTime: %lf\n", waitingTime);

			//draw service time from exponential distribution
			sDrawTime = drawRandExpDist(*(double*)serviceRate);
			//*serverProcessTime += sDrawTime;
			sleepTime.tv_sec = 0;
			sleepTime.tv_nsec = sDrawTime * 1000000000L;
			//printf("sDrawTime %lf\n", sDrawTime);
			
			//sleep for that much time
			nanosleep(&sleepTime, NULL);

			free(aCustomer);
			customerCount++; 

			//lock service and wait time variables
			pthread_mutex_lock(&t2Mutex);
			//calculate service time mean
			(*serviceTimeCount)++;
			onlineAvg(mean_serviceTime, serviceTimeX2, sDrawTime, *serviceTimeCount);

			//calculate wait time mean
			(*waitingTimeCount)++;
			onlineAvg(mean_waitingTime, waitingTimeX2, waitingTime, *waitingTimeCount);
			pthread_mutex_unlock(&t2Mutex);

			//printf("Customer %d Serviced\n", customerCount);

			//calculate serviceTime for server utilization
			gettimeofday(&serviceEndTime, NULL);
			serviceTime = serviceEndTime.tv_usec - serviceStartTime.tv_usec;

			pthread_mutex_lock(&t2SMutex);
			*serverProcessTime += sDrawTime;
			pthread_mutex_unlock(&t2SMutex);
		}
		//2. an empty queue: wait for signal
		else {
			//printf("qHead is null\n");
			pthread_cond_wait(&queueEmpty, &qMutex);
			pthread_mutex_unlock(&qMutex);
		}
		
	}

	//custemers serviced count hit CUSTOMERCOUNT
	serviceThreadDone = 1; //signal to threads 1 and 3 to end service
	pthread_exit(0);
}

void* thread3(void* queueData) {
	//printf("Enter Thread3\n")

	thread3_data* thrd3_data = (thread3_data*)queueData;
	double* mean_queueTime = thrd3_data->qMean;
	double* queueTimeX2 = thrd3_data->qX2;
	int* queueLengthCount = thrd3_data->qCount;
	
	while (serviceThreadDone == 0) {
		//calculate queue mean
		(*queueLengthCount)++;
		//pthread_mutex_lock(&qMutex);
		onlineAvg(mean_queueTime, queueTimeX2, (double)qLength, *queueLengthCount);
		//pthread_mutex_unlock(&qMutex);
		sleep(0.005);
	}
}

void onlineAvg(double* avg, double* sumX2, double input, int size) {
	//printf("INPUT: %lf, SIZE: %d\n", input, size);
	double sumX = 0.0;
	if (size > 1) {
		sumX = *avg * ((double)size - 1.0);
	}
	sumX += input;
	
	*avg = sumX / (double)size;
	*sumX2 += input * input;
	//printf("ONLINE SUM: %lf AVG: %lf SUMX2: %lf\n", sumX, *avg, *sumX2);
}

void offlineStd(double* avg, double* sumX2, double* std, int size) {
	double sumX; 
	sumX = *avg * size; 

	*std = sqrt(((*sumX2) - (*avg) * (*avg) * size) / (size - 1));
}


void printStatistics(double mean_interArrival, double std_interArrival, double mean_waiting, double std_waiting,
	double mean_serviceTime, double std_serviceTime, double mean_queueLen, double std_queueLen, double serverUtilization)
{
	printf("Inter-arrival time Avg/Std: %lf / %lf\n", mean_interArrival, std_interArrival);
	printf("Waiting time Avg/Std: %lf / %lf\n", mean_waiting, std_waiting);
	printf("Service time Avg/Std: %lf / %lf\n", mean_serviceTime, std_serviceTime);
	printf("Queue Length Avg/Std: %lf / %lf\n", mean_queueLen, std_queueLen);
	printf("Server Utilization: %lf\n", serverUtilization);
}

