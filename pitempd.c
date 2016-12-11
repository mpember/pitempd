/*
 *	pitempd.c:
 *	Simple daemon for monitoring multiple temperature sensors
 *	Amended by technion@lolware.net
 *  And then tweaked by mpember@mpember.net.au
 *  Version 1.0.0
 *     * Initial Release
 *  Version 1.1.0
 *     + Support for configuration file
 *     + Support for debug mode
 *  Version 1.2.0
 *     + Support for Thingpeak API
 *  Version 1.2.1
 *     + Move sensor settings into own section of config file
 *  Version 1.3.0
 *     + Support for Pushover API
 *     + Support for triggering alerts based on temperature
 *  Version 1.3.1
 *     ! Detect missing Pushover / Thingspeak tokens
 *  Version 1.3.2
 *     + Offline mode to avoid excess web traffic
 *  Version 1.3.3
 *     + Adjust Pushover message to identify sensor
 *     + Modify timing for inerval checks
 *  Version 1.4.0
 *     + Wait for second extreme result before accepting reading
 *  Version 1.5.0
 *     + Add Offset setting to adjust reading
 */

#include <wiringPi.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include <curl/curl.h>

#include "minini/minIni.h"
#define sizearray(a)  (sizeof(a) / sizeof((a)[0]))

#define MAXTIMINGS 85

static char inifile[] = "/etc/pitempd.conf";

static bool _DEBUG = false;
static bool _OFFLINE = false;
static int GPIO_PIN[2] = {-1,-1};
static char GPIO_LABEL[2][20];
static float trigger_high[2] = {0,0};
static float trigger_low[2] = {0,0};
static float offset_temp[2] = {0,0};
static float offset_hum[2] = {0,0};
float trigger_state[2] = {0,0};
float stats[4] = {0,0,0,0};

int submit_pushover(char message[100]) {
	CURL *curl;
	CURLcode res;
	char token[32];
	char user[32];
	char postdata[100];
	int priority;

	if (_OFFLINE) {
		if (_DEBUG) printf("Pushover Message: %s\n", message);
		return 0;
	}

	if (
		ini_gets("pushover", "token", "", token, sizearray(token), inifile) == 0 ||
		ini_gets("pushover", "user", "", user, sizearray(user), inifile) == 0
	) return 0;

	priority = ini_getl("pushover", "priority", 0, inifile);

	/* In windows, this will init the winsock stuff */
	curl_global_init(CURL_GLOBAL_ALL);

	/* get a curl handle */
	curl = curl_easy_init();
	if(curl) {
		curl_easy_setopt(curl, CURLOPT_URL, "https://api.pushover.net/1/messages.json");
		// sprintf(postdata, "token=%s&user=%s&priority=%i&message=%s", token, user, priority, curl_easy_escape(curl, message, 0));
		sprintf(postdata, "token=%s&user=%s&message=%s", token, user, curl_easy_escape(curl, message, 0));
		if (_DEBUG) printf("POST Data: %s\n", postdata);

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postdata);

		/* Perform the request, res will get the return code */
		res = curl_easy_perform(curl);

		/* Check for errors */
		if(res != CURLE_OK)
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));

		/* always cleanup */
		curl_easy_cleanup(curl);
	}

	curl_global_cleanup();
	return 0;
}

int submit_thingspeak(void) {
	CURL *curl;
	CURLcode res;
	char token[17];
	char postdata[100];

	if (_OFFLINE || ini_gets("thingspeak", "token", "", token, sizearray(token), inifile) == 0) return 0;

	/* In windows, this will init the winsock stuff */
	curl_global_init(CURL_GLOBAL_ALL);

	/* get a curl handle */
	curl = curl_easy_init();

	if(curl) {
		curl_easy_setopt(curl, CURLOPT_URL, "https://api.thingspeak.com/update");
		sprintf(postdata, "key=%s&field1=%.2f&field2=%.2f&field3=%.2f&field4=%.2f", token, stats[0], stats[1], stats[2], stats[3]);
		if (_DEBUG) printf("POST Data: %s\n", postdata);

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postdata);

		/* Perform the request, res will get the return code */
		res = curl_easy_perform(curl);
		/* Check for errors */
		if(res != CURLE_OK)
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));
		/* always cleanup */ 
		curl_easy_cleanup(curl);
	}

	curl_global_cleanup();
	return 0;
}

static uint8_t sizecvt(const int read) {
	/* digitalRead() and friends from wiringpi are defined as returning a value
	< 256. However, they are returned as int() types. This is a safety function */

	if (read > 255 || read < 0) {
		if (_DEBUG) printf("Invalid data from wiringPi library\n");
		exit(EXIT_FAILURE);
	}
	return (uint8_t)read;
}

static int write_state(int SENSOR) {
	FILE *fp;
	char state_file[20] = "";

	sprintf(state_file, "/tmp/temp%c.state", SENSOR+'0');

	if ((fp=fopen(state_file, "w")) == NULL)
		printf("Sensor %i: State file failed to open\n", SENSOR);
	else {
		fprintf(fp, "%.2f %.2f", stats[(SENSOR*2) + 0], stats[(SENSOR*2) + 1]);
		fflush(fp);
		fclose(fp);
	}

	return 0;
}

static int read_dht22_dat(int SENSOR) {
	static int dht22_dat[5] = {0,0,0,0,0};

	uint8_t laststate = HIGH;
	uint8_t counter = 0;
	uint8_t j = 0, i;

	dht22_dat[0] = dht22_dat[1] = dht22_dat[2] = dht22_dat[3] = dht22_dat[4] = 0;

	if (_DEBUG) printf("Reading %s sensor on GPIO pin %i\n", GPIO_LABEL[SENSOR], GPIO_PIN[SENSOR]);
	if (GPIO_PIN[SENSOR] < 0) return 0;

	// pull pin down for 18 milliseconds
	pinMode(GPIO_PIN[SENSOR], OUTPUT);
	digitalWrite(GPIO_PIN[SENSOR], HIGH);
	delay(10);
	digitalWrite(GPIO_PIN[SENSOR], LOW);
	delay(18);

	// then pull it up for 40 microseconds
	digitalWrite(GPIO_PIN[SENSOR], HIGH);
	delayMicroseconds(40);

	// prepare to read the pin
	pinMode(GPIO_PIN[SENSOR], INPUT);

	// detect change and read data
	for (i=0; i < MAXTIMINGS; i++) {
		counter = 0;

		while (sizecvt(digitalRead(GPIO_PIN[SENSOR])) == laststate) {
			counter++;
			delayMicroseconds(1);
			if (counter == 255) break;
		}

		laststate = sizecvt(digitalRead(GPIO_PIN[SENSOR]));

		if (counter == 255) break;

		// ignore first 3 transitions
		if ((i >= 4) && (i%2 == 0)) {
			// shove each bit into the storage bytes
			dht22_dat[j/8] <<= 1;
			if (counter > 16) dht22_dat[j/8] |= 1;
			j++;
		}
	}

	// check we read 40 bits (8bit x 5 ) + verify checksum in the last byte
	// print it out if data is good
	if ((j >= 40) && 
		(dht22_dat[4] == ((dht22_dat[0] + dht22_dat[1] + dht22_dat[2] + dht22_dat[3]) & 0xFF)) ) {
			float t, h;
			h = (float)dht22_dat[0] * 256 + (float)dht22_dat[1];
			h /= 10;
			t = (float)(dht22_dat[2] & 0x7F)* 256 + (float)dht22_dat[3];
			t /= 10.0;
			if ((dht22_dat[2] & 0x80) != 0)	t *= -1;

			if (offset_temp[SENSOR] != 0) t = t + offset_temp[SENSOR];
			if (offset_hum[SENSOR] != 0) h = h + offset_hum[SENSOR];

			if (t - stats[SENSOR*2] > 5 || stats[SENSOR*2] - t > 5) {
				stats[SENSOR*2] = t;
				stats[(SENSOR*2) + 1] = h;
				if (_DEBUG) printf("Sensor %i: Temperature difference too big, waiting for second try\n", SENSOR);
				return 0;
			}

			if (trigger_state[SENSOR] < 1 && t > stats[SENSOR*2] && t > trigger_high[SENSOR]) {
				trigger_state[SENSOR] = 1;
				char pushover_message[100];
				sprintf(pushover_message, "Sensor %s:\n%.2f *C exceeds upper limit of %.2f *C\n", GPIO_LABEL[SENSOR], t, trigger_high[SENSOR]);
				submit_pushover(pushover_message);
			} else if (trigger_state[SENSOR] > -1 && (stats[SENSOR*2] == 0 || t < stats[SENSOR*2]) && t < trigger_low[SENSOR]) {
				trigger_state[SENSOR] = -1;
				char pushover_message[100];
				sprintf(pushover_message, "Sensor %s:\n%.2f *C exceeds lower limit of %.2f *C\n", GPIO_LABEL[SENSOR], t, trigger_low[SENSOR]);
				submit_pushover(pushover_message);
			}
			
			stats[SENSOR*2] = t;
			stats[(SENSOR*2) + 1] = h;

		return 1;
	}
	else {
		if (_DEBUG) printf("Sensor %i: Data not good, skip\n", SENSOR);
		return 0;
	}
}

int main (int argc, char *argv[]) {
	int k;
	char name[20];
	int value;

	if (argc == 2) {
		if (strcmp(argv[1], "-d") == 0) _DEBUG = true;
		if (strcmp(argv[1], "-o") == 0) _OFFLINE = true;
	} else if (argc == 3) {
		if (strcmp(argv[1], "-d") == 0 || strcmp(argv[2], "-d") == 0) _DEBUG = true;
		if (strcmp(argv[1], "-o") == 0 || strcmp(argv[2], "-o") == 0) _OFFLINE = true;
	}

	if (_DEBUG)
		printf ("Raspberry Pi wiringPi DHT22 service\n");

	for (k = 0; ini_getkey("sensors", k, name, 20, inifile) > 0; k++) {
		strcpy(GPIO_LABEL[k], name);
		value = ini_getl("sensors", name, -1, inifile);
		GPIO_PIN[k] = value;
		trigger_high[k] = (float)ini_getf(name, "upper", 25, inifile);
		trigger_low[k] = (float)ini_getf(name, "lower", 20, inifile);
		offset_temp[k] = (float)ini_getf(name, "offset_temp", 0, inifile);
		offset_hum[k] = (float)ini_getf(name, "offset_hum", 0, inifile);
		if (_DEBUG) {
			printf("%s = GPIO %d\n", name, value);
			printf("%s = High %.2f\n", name, trigger_high[k]);
			printf("%s = Low %.2f\n", name, trigger_low[k]);
			printf("%s = Offset Temp %.2f\n", name, offset_temp[k]);
			printf("%s = Offset Hum %.2f\n", name, offset_hum[k]);
		}
	}

	if (!_DEBUG) {
		pid_t process_id = 0;
		pid_t sid = 0;

		process_id = fork();
		if (process_id < 0) {
			printf("fork failed!\n");
			// Return failure in exit status
			exit(1);
		}

		if (process_id > 0) {
			printf("process_id of child process %d\n", process_id);
			// return success in exit status
			exit(0);
		}

		//unmask the file mode
		umask(0);
		//set new session
		sid = setsid();
		if (sid < 0) {
			// Return failure
			exit(1);
		}

		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);

		if (setuid(getuid()) < 0) {
			perror("Dropping privileges failed\n");
			exit(EXIT_FAILURE);
		}
	}
	else
		printf ("Sensor count = %i\n", k) ;

	if (wiringPiSetup () == -1)
		exit(EXIT_FAILURE);

	int _state = 0;

	while (1) {
		if (_state == 10)
			// Reading external sensor
			_state = 10 + read_dht22_dat(1);
		else if (_state == 1)
			// Reading internal sensor
			_state = (10 * read_dht22_dat(0))+1;
		else {
			// Reading both sensors
			_state = (10 * read_dht22_dat(0))+read_dht22_dat(1);
		}

		if (_state == 11) {
			// Successful read of both sensors
			// printf("Sensor Data: %.2f *C %.2f %% %.2f *C %.2f %% \n", stats[0], stats[1], stats[2], stats[3]);
			printf("Sensor Data (%s): %.2f *C %.2f %% \n", GPIO_LABEL[0], stats[0], stats[1]);
			printf("Sensor Data (%s): %.2f *C %.2f %% \n", GPIO_LABEL[1], stats[2], stats[3]);
			submit_thingspeak();

			write_state(0);
			write_state(1);

			_state = 0;
			if (_DEBUG) // break;
				delayMicroseconds(10000000);
			else
				delayMicroseconds(300000000);
			// sleep(300);
		}
		else {
			// One of the sensors returned faulty data, retry after 50ms
			delayMicroseconds(100);
		}
	}

	return 0;
}