/* RICO Interface program */

#include <stdio.h>	// for FILE
#include <stdlib.h>	// for timeval
#include <string.h>	// for strlen etc
#include <time.h>	// for ctime
#include <sys/types.h>	// for fd_set
// #include <sys/socket.h>
// #include <netinet/in.h>
#include <netdb.h>	// for sockaddr_in 
#include <fcntl.h>	// for O_RDWR
#include <termios.h>	// for termios
#include <unistd.h>		// for getopt
#include <errno.h>		// for Linux
#include <signal.h>

#define REVISION "$Revision: 1.4 $"
/* 1.0 11/02/2008 Initial version copied from Elster 1.3
   1.2 13/02/2008 Handles kw and kwh commands
   1.3 09/08/2009 Netport support hostname:port
   1.4 18/03/2011 Add w: meaning display value as watts not kilowatts (mlutiply by 1000 and 0 decimal places
*/

static char* id="@(#)$Id: rico.c,v 1.4 2011/05/09 18:08:48 martin Exp $";

#define PORTNO 10010
#define PROGNAME "Rico"
#define LOGON "rico"
#define LOGFILE "/tmp/rico%d.log"
#define SERIALNAME "/dev/ttyAM0"	/* although it MUST be supplied on command line */
#define BAUD B9600

// Severity levels.  ERROR and FATAL terminate program
#define INFO	0
#define	WARN	1
#define	ERROR	2
#define	FATAL	3
// Socket retry params
#define NUMRETRIES 3
#define RETRYDELAY	1000000	/* microseconds */
// Serial retry params
#define SERIALNUMRETRIES 10
#define SERIALRETRYDELAY 1000000 /*microseconds */
// Set to if(0) to disable debugging
#define DEBUG if(debug >= 1)
#define DEBUG2 if(debug >=2)
// If defined, don't open the serial device
// #define DEBUGCOMMS

/* SOCKET CLIENT */

/* Command line params: 
1 - device name
2 - device timeout. Default to 60 seconds
3 - optional 'nolog' to suppress writing to local logfile
options:
-3 first value (kw)
-8 second value (kg)
-8 third value (kwh)
-f CO2 scale factor
*/

#ifndef linux
extern
#endif
int errno;  

// Procedures in this file
int openSerial(const char * name, int baud, int parity, int databits, int stopbits);  // return fd
void closeSerial(int fd);  // restore terminal settings
void sockSend(const char * msg);	// send a string
int sendSerials(int fd, unsigned char *data, int len);
int sendSerial(int fd, unsigned char data);
int processSocket(int fd, float factor);			// process server message
void logmsg(int severity, char *msg);	// Log a message to server and file
void usage(void);					// standard usage message
char * getversion(void);
void ricosend (int fd, int display, float value, int decimals);
void catcher(int sig);			// Signal catcher needed for SIGPIPE

/* GLOBALS */
FILE * logfp = NULL;
int sockfd = 0;
int debug = 0;
int noserver = 0;		// prevents socket connection when set to 1
int controllernum = -1;	//	only used in logon message
char buffer[256];		// For messages
char * serialName = SERIALNAME;
int bus = 1;
int watts = 0;		// Interpret the kw figure as watts instead

/********/
/* MAIN */
/********/
int main(int argc, char *argv[])
// arg1: serial device file
// arg2: optional timeout in seconds, default 90
// arg3: optional 'nolog' to carry on when filesystem full
{
    int commfd;
	int nolog = 0;

    struct sockaddr_in serv_addr;
    struct hostent *server;
	time_t next;

	int run = 1;		// set to 0 to stop main loop
	fd_set readfd; 
	int numfds;
	struct timeval timeout;
	int tmout = 690;		//seconds between messages
	int logerror = 0;
	int online = 1;		// used to prevent messages every minute in the event of disconnection
	float factor = 0.43;		// CO2 kwh -> kg conversion
	float value;
	int display = 0, decimals = 0;
	int option; 
	int baud = BAUD;
	// Command line arguments
	
	// optind = -1;
	opterr = 0;
	while ((option = getopt(argc, argv, "b:dt:slV1:2:3:f:4:5:6:7:8:D:Lw")) != -1) {
		switch (option) {
		case 'b': bus = atoi(optarg); break;
		case 's': noserver = 1; break;
		case 'l': nolog = 1; break;
		case '?': usage(); exit(1);
		case 't': tmout = atoi(optarg); break;
		case 'd': debug++; break;
		case '1': display = 1; value = atof(optarg); break;
		case '2': display = 2; value = atof(optarg); break;
		case '3': display = 3; value = atof(optarg); break;
		case '4': display = 4; value = atof(optarg); break;
		case '5': display = 5; value = atof(optarg); break;
		case '6': display = 6; value = atof(optarg); break;
		case '7': display = 7; value = atof(optarg); break;
		case '8': display = 8; value = atof(optarg); break;
		case 'L': baud = B2400; break;
		case 'f': factor = atof(optarg); break;
		case 'D': decimals = atoi(optarg); break;
		case 'w': watts = 1; break;
		case 'V': printf("Version %s %s\n", getversion(), id); exit(0);
		}
	}
	
	DEBUG fprintf(stderr, "Debug on. optind %d argc %d Bus = %d Display = %d\n", optind, argc, bus, display);
	
	if (optind < argc) serialName = argv[optind];		// get seria/device name: parameter 1
	optind++;
	if (optind < argc) controllernum = atoi(argv[optind]);	// get optional controller number: parameter 2
	
	sprintf(buffer, LOGFILE, controllernum);
	
	if (!nolog) if ((logfp = fopen(buffer, "a")) == NULL) logerror = errno;
	
	// There is no point in logging the failure to open the logfile
	// to the logfile, and the socket is not yet open.

	sprintf(buffer, "STARTED %s on %s as %d timeout %d %s", argv[0], serialName, controllernum, tmout, nolog ? "nolog" : "");
	logmsg(INFO, buffer);
	
	// Set up socket 
	if (!noserver) {
		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0) 
			logmsg(FATAL, "FATAL " PROGNAME " Creating socket");
		server = gethostbyname("localhost");
		if (server == NULL) {
			logmsg(FATAL, "FATAL " PROGNAME " Cannot resolve localhost");
		}
		bzero((char *) &serv_addr, sizeof(serv_addr));
		serv_addr.sin_family = AF_INET;
		bcopy((char *)server->h_addr, 
			 (char *)&serv_addr.sin_addr.s_addr,
			 server->h_length);
		serv_addr.sin_port = htons(PORTNO);
		if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
			sockfd = 0;
			logmsg(ERROR, "ERROR " PROGNAME " Connecting to socket");
		}	
		
		if (flock(commfd, LOCK_EX | LOCK_NB) == -1) {
			sprintf(buffer, "FATAL " PROGNAME " is already running, cannot start another one on %s", serialName);
			logmsg(FATAL, buffer);
		}
	
		// Logon to server
		sprintf(buffer, "logon rico %s %d %d", getversion(), getpid(), controllernum);
		sockSend(buffer);
	}
	else	sockfd = 1;		// noserver: use stdout
	
		// Open serial port
	if ((commfd = openSerial(serialName, baud, 0, CS8, 1)) < 0) {
		sprintf(buffer, "ERROR " PROGNAME " %d Failed to open %s: %s", controllernum, serialName, strerror(errno));
#ifdef DEBUGCOMMS
		logmsg(INFO, buffer);			// FIXME AFTER TEST
		printf("Using stdio\n");
		commfd = 0;		// use stdin
#else
		logmsg(FATAL, buffer);
#endif
	}

	// If we failed to open the logfile and were NOT called with nolog, warn server
	// Obviously don't use logmsg!
	if (logfp == NULL && nolog == 0) {
		sprintf(buffer, "event WARN " PROGNAME " %d could not open logfile %s: %s", controllernum, LOGFILE, strerror(logerror));
		sockSend(buffer);
	}
		
	numfds = (sockfd > commfd ? sockfd : commfd) + 1;		// nfds parameter to select. One more than highest descriptor

	if (display) {	// command line value
		ricosend(commfd, display, value, decimals);
		fprintf(stderr, "\n");
		close(commfd);
		return 0;		// and exit
	}
	
	// Main Loop
	signal(SIGPIPE, catcher);
	FD_ZERO(&readfd); 
	DEBUG fprintf(stderr, "Now is %d next is %d\n", time(NULL), next);
	while(run) {
		timeout.tv_sec = tmout;
		timeout.tv_usec = 0;
		FD_SET(sockfd, &readfd);
		if (select(numfds, &readfd, NULL, NULL, &timeout) == 0) {	// select timed out. Bad news 
			if (online == 1) {
				logmsg(WARN, "WARN " PROGNAME " No data for last period");
				online = 0;	// Don't send a message every minute from now on
			}
			continue;
		}
		if ((noserver == 0) && FD_ISSET(sockfd, &readfd)) {
			online = 1;
			run = processSocket(commfd, factor);	// the server may request a shutdown by setting run to 0
		}
	}			
	logmsg(INFO,"INFO " PROGNAME " Shutdown requested");
	close(sockfd);
	closeSerial(commfd);

	return 0;
}

/*********/
/* USAGE */
/*********/
void usage(void) {
	printf("Usage: rico [-lsd] [-f xx.xx] [-bX] [-V] /dev/ttyname controllernum\n");
	printf("-l: no log  -s: no server  -d: debug on\n -V: version -f: CO2 scale factor -3,5,8: test value\n");
	printf("-w watts instead of kw -L low speed 2400 baud -bX bus X\n");
	return;
}

/**********/
/* LOGMSG */
/**********/
void logmsg(int severity, char *msg)
// Write error message to logfile and socket if possible and abort program for ERROR and FATAL
// Truncate total message including timestamp to 200 bytes.

// Globals used: sockfd logfp

// Due to the risk of looping when you call this routine due to a problem writing on the socket,
// set sockfd to 0 before calling it.

{
	char buffer[200];
	time_t now;
	if (strlen(msg) > 174) msg[174] = '\0';		// truncate incoming message string
	now = time(NULL);
	strcpy(buffer, ctime(&now));
	buffer[24] = ' ';	// replace newline with a space
	strcat(buffer, msg);
	strcat(buffer, "\n");
	if (logfp) {
		fputs(buffer, logfp);
		fflush(logfp);
	} 
	if (sockfd > 0) {
		strcpy(buffer, "event ");
		strcat(buffer, msg);
		sockSend(buffer);
	}
    if (severity > WARN) {		// If severity is ERROR or FATAL terminate program
		if (logfp) fclose(logfp);
		if (sockfd) close(sockfd);
		exit(severity);
	}
}

// Static data
struct termios oldSettings, newSettings; 

/**************/
/* OPENSERIAL */
/**************/
int openSerial(const char * name, int baud, int parity, int databits, int stopbits) {
/* open serial device; return file descriptor or -1 for error (see errno) */
	int fd, res;
	
	if (strchr(name, ':')) 
		return openSocket(name);
	
	if ((fd = open(name, O_RDWR | O_NOCTTY)) < 0) return fd;	// an error code
	
	tcgetattr(fd, &oldSettings);
	bzero(&newSettings, sizeof(newSettings));
	// Control Modes
	newSettings.c_cflag = /* CRTSCTS  | */ databits | CLOCAL | CREAD;
	if (stopbits == 2) 
		newSettings.c_cflag |= CSTOPB;
	// input modes
	newSettings.c_iflag = IGNPAR;	//input modes
	newSettings.c_oflag = 0;		// output modes
	newSettings.c_lflag = 0;		// local flag
	newSettings.c_cc[VTIME] = 0; // intercharacter timer */
	newSettings.c_cc[VMIN] = 0;	// non-blocking read */
	tcflush(fd, TCIFLUSH);		// discard pending data
 	if (cfsetspeed(&newSettings, baud))
		perror("Setting serial port");
	if((res = tcsetattr(fd, TCSANOW, &newSettings)) < 0) {
		close(fd);	// if there's an error setting values, return the error code
		return res;
	}
	return fd;
}

/**************/
/* OPENSOCKET */
/**************/
// Return an open fd or -1 for error
// Expects name to be hostname:portname where either can be a name or numeric.
// Need to avoid overwriting/alterng input string in case of re-use
int openSocket(char * fullname) {
	char * portname;
	char name[64];
	int fd;
	struct sockaddr_in serv_addr;
    struct hostent *server;
	struct servent * portent;
	int port;
	int firstime = 1;
	
	portname = strchr(fullname, ':');
	if (!portname) return -1;	// No colon in hostname:portname
	if (portname - fullname > 64) {
		sprintf(buffer, "ERROR " PROGNAME " port name is too long: '%s", fullname);
		logmsg(ERROR, buffer);
		return -1;
	}
	strncpy(name, fullname, portname - fullname);
	name[portname - fullname] = 0;
	portname++;		// Now portname point to port part and name is just host part.
	
	server = gethostbyname(name);
	if (!server) {
		sprintf(buffer,"ERROR " PROGNAME " Cannot resolve hostname %s", name);
		logmsg(ERROR, buffer);	// Won't return
		return -1;
	}
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	bcopy((char *)server->h_addr, 
		  (char *)&serv_addr.sin_addr.s_addr,
		  server->h_length);
	port = atoi(portname);		// Try it as a number first
	if (!port) {
		portent = getservbyname(portname, "tcp");
		if (portent == NULL) {
			sprintf(buffer,"ERROR " PROGNAME " Can't resolve port: %s", portname);
			logmsg(ERROR, buffer);	// Won't return
			return -1;
		}
		serv_addr.sin_port = portent->s_port;
	}
	else
		serv_addr.sin_port = htons(port);
		
	DEBUG fprintf(stderr, "Connect to %s:%d ", name, ntohs(serv_addr.sin_port));
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		sprintf(buffer, "ERROR " PROGNAME " Can't create socket: %s", strerror(errno));
		logmsg(ERROR, buffer);		// Won't return
		return -1;
	}
	DEBUG fprintf(stderr, "About to connect on %d ..", fd);
	// If we can't connect due to timeout (far end not ready) keep trying forever
	while (connect(fd, (struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
		DEBUG fprintf(stderr, "Connect fd%d ... sin_family %d sin_port %d sin_addr 0x%x ", fd, 
					  serv_addr.sin_family, serv_addr.sin_port, serv_addr.sin_addr.s_addr);
		if (firstime && errno == ETIMEDOUT) {
			firstime = 0;
			sprintf(buffer,"WARN " PROGNAME " Error connecting to remote serial %s - will keep trying %d (%s)", 
					fullname, errno, strerror(errno));
			logmsg(WARN, buffer);
		}
		
		if (errno != ETIMEDOUT) {
			sprintf(buffer,"WARN " PROGNAME " Error other than time out connecting to remote serial: %s", strerror(errno));
			logmsg(WARN, buffer);
			sleep(60);
		}
		if (errno == ETIMEDOUT) {
			fd = close(fd);		// necessary in order to reuse file desciptors.
			if (fd < 0){
				sprintf(buffer, "WARN " PROGNAME " Can't close socket: %s", strerror(errno));
				logmsg(WARN, buffer);		// Won't return
				return -1;
			}
			fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0) {
				sprintf(buffer, "WARN " PROGNAME " Can't create socket after closing: %s", strerror(errno));
				logmsg(WARN, buffer);		// Won't return
				return -1;
			}
		}
	}
	DEBUG fprintf(stderr, "Connected on FD%d\n", fd);
	return fd;
}

/***************/
/* CLOSESERIAL */
/***************/
void closeSerial(int fd) {
// Restore old serial port settings
	tcsetattr(fd, TCSANOW, &oldSettings);
	close(fd);
}

/************/
/* SOCKSEND */
/************/
void sockSend(const char * msg) {
// Send the string to the server.  May terminate the program if necessary
	short int msglen, written;
	int retries = NUMRETRIES;
	
	if(noserver) {	// shortcut when in test mode
		puts(msg);
		return;
	}

	msglen = strlen(msg);
	written = htons(msglen);

	if (write(sockfd, &written, 2) != 2) { // Can't even send length ??
		sockfd = 0;		// prevent logmsg trying to write to socket!
		logmsg(ERROR, "ERROR " PROGNAME " Can't write a length to socket");
	}
	while ((written = write(sockfd, msg, msglen)) < msglen) {
		// not all written at first go
			msg += written; msglen =- written;
			printf("Only wrote %d; %d left \n", written, msglen);
			if (--retries == 0) {
				logmsg(WARN, "WARN " PROGNAME " Timed out writing to server"); 
				return;
			}
			usleep(RETRYDELAY);
	}
}

/*****************/
/* PROCESSSOCKET */
/*****************/
int processSocket(int fd, float factor){
// Deal with commands from MCP.  Return to 0 to do a shutdown
	short int msglen, numread;
	char buffer[32], buffer2[192];	// about 128 is good but rather excessive since longest message is 'truncate'
	char * cp = &buffer[0];
	int retries = NUMRETRIES;
	int n;
	float val;
		
	if (read(sockfd, &msglen, 2) != 2) {
		logmsg(WARN, "WARN " PROGNAME " Failed to read length from socket");
		return 1;
	}
	msglen =  ntohs(msglen);
	while ((numread = read(sockfd, cp, msglen)) < msglen) {
		cp += numread;
		msglen -= numread;
		if (--retries == 0) {
			logmsg(WARN, "WARN " PROGNAME " Timed out reading from server");
			return 1;
		}
		usleep(RETRYDELAY);
	}
	cp[numread] = '\0';	// terminate the buffer 
	
	if (strcmp(buffer, "exit") == 0)
		return 0;	// Terminate program
	if (strcmp(buffer, "Ok") == 0)
		return 1;	// Just acknowledgement
	if (strcmp(buffer, "truncate") == 0) {
		if (logfp) {
		// ftruncate(logfp, 0L);
		// lseek(logfp, 0L, SEEK_SET);
			freopen(NULL, "w", logfp);
			logmsg(INFO, "INFO " PROGNAME " Truncated log file");
		} else
			logmsg(INFO, "INFO " PROGNAME " Log file not truncated as it is not open");
		return 1;
	}
	if (strcmp(buffer, "debug 0") == 0) {	// turn off debug
		debug = 0;
		return 1;
	}
	if (strcmp(buffer, "debug 1") == 0) {	// enable debugging
		debug = 1;
		return 1;
	}
	if (strcmp(buffer, "debug 2") == 0) {	// enable debugging
		debug = 2;
		return 1;
	}
	if (strcmp(buffer, "help") == 0) {
		strcpy(buffer2, "INFO Commands are: debug 0|1, exit, truncate, kw or kwh or disp N val [places]");
		logmsg(INFO, buffer2);
		return 1;
	}
	if (strncmp(buffer, "w ", 2) == 0) {
		n = sscanf(buffer+1, "%f", &val);
		if (n != 1) {
			logmsg(WARN, "WARN " PROGNAME " failed to get w (watts) value");
			return 1;
		}
		ricosend(fd, 3, val * 1000.0, 0);	// Send watts to display 3, with 3 decimal places
		return 1;
	}
	if (strncmp(buffer, "kw ", 3) == 0) {
		n = sscanf(buffer+2, "%f", &val);
		if (n != 1) {
			logmsg(WARN, "WARN " PROGNAME " failed to get kw value");
			return 1;
		}
		if (watts)
			ricosend(fd, 3, val * 1000.0, 0);
		else				
			ricosend(fd, 3, val, 3);	// Send kw to display 3, with 3 decimal places
		return 1;
	}
	if (strncmp(buffer, "kwh ", 4) == 0) {
		n = sscanf(buffer+3, "%f", &val);
		if (n != 1) {
			logmsg(WARN, "WARN " PROGNAME " failed to get kwh value");
			return 1;
		}
		ricosend(fd, 5, val, -1);		// send kwh to display 5 ... 
		ricosend(fd, 8, val * factor, -1);	// and CO2 to display 8 
					// with decimal place located automatically
		return 1;
	}
	if (strncmp(buffer, "disp ", 5) == 0) {
		int num, decimals;
		decimals = 3;
		n = sscanf(buffer+5, "%d %f %d", &num, &val, &decimals);
		if (n != 2 && n!= 3) {
			logmsg(WARN, "WARN " PROGNAME " failed to get display number and value");
			return 1;
		}
		if (num == 2 && watts)	// WATTS - frig for Ecotech since MCP doesn't send kw
			ricosend(fd, num, val * 1000.0, 3);
		else
			ricosend(fd, num, val, decimals);	// Send value to specified display, with 3 decimal places
		return 1;
	}
	strcpy(buffer2, "INFO " PROGNAME " Unknown message from server: ");
	strcat(buffer2, buffer);
	logmsg(INFO, buffer2);	// Risk of loop: sending unknown message straight back to server
	
	return 1;	
};

/**************/
/* GETVERSION */
/**************/
char *getversion(void) {
// return pointer to version part of REVISION macro
	static char version[10] = "";	// Room for xxxx.yyyy
	if (!strlen(version)) {
		strcpy(version, REVISION+11);
		version[strlen(version)-2] = '\0';
	}
return version;
}

/************/
/* RICOSEND */
/************/
void ricosend (int fd, int display, float value, int decimals) {
	// Send the value to the display number
	// If decimals is less than 0, automatically determine it 
	union {
		unsigned char raw[13];
		struct {
			unsigned char N;
			unsigned char bus;
			unsigned char displ;
			char value[9];
			unsigned char checksum;
		} s;
	} data;
	
	int i, sum;
	data.s.N = 'N';	
	data.s.bus = bus;	// it's a global. Sorry.
	char * format;
	DEBUG fprintf(stderr,"Ricosend FD = %d %d %f %d digits. ", fd, display, value, decimals);
	if (display < 1 || display > 8) {
		sprintf(buffer, "WARN " PROGNAME " Display is not in range 1 to 8: %d", display);
		logmsg(WARN, buffer);
		return;
	}
	data.s.displ = display;
	if (decimals < 0) 
		if (value > 99999.99)
			decimals = 0;
		else if (value > 9999.99)
			decimals = 1;
		else
			decimals = 2;
	
	switch(decimals) {
		case 0: format = "%9.0f"; break;
		case 1: format = "%9.1f"; break;
		case 2: format = "%9.2f"; break;
		case 3: format = "%9.3f"; break;
		default: format = "%9f"; break;
	}
	sprintf(data.s.value, format, value);
	sum = 0;
	for (i = 0; i < 12; i++) sum += data.raw[i];
	data.s.checksum = sum;
	
	DEBUG2 {
		fprintf(stderr, "Sum = %x ", sum);
		fprintf(stderr, "Sending ");
		for (i = 0; i < 13; i++) fprintf(stderr, "%02x ", data.raw[i]);
		fprintf(stderr, " '");
		for (i = 0; i < 9; i++) fprintf(stderr, "%c", data.s.value[i]);
		fprintf(stderr, "'\n");
	}
	for (i = 0; i < 13; i++) sendSerial(fd, data.raw[i]);
	
	int ret;
	// The RS232 port will return ok '<' or fail within 1/10th second.  The RS422 doesn't.
//	usleep(100000);  // 100mSec
	fd_set readfd;
	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;
	FD_ZERO(&readfd);
	FD_SET(fd, &readfd);
	DEBUG fprintf(stderr, "Before FD=%d fdset=%x ", fd, readfd);
	if (select(fd + 1, &readfd, NULL, NULL, &timeout) == 0) {	// select timed out. Bad news 
		DEBUG fprintf(stderr, "No response\n");
	}
	else {
		DEBUG fprintf(stderr, "FD readable .. ");
		ret=read(fd, data.raw, 13);
		DEBUG2 {
			fprintf(stderr, "Read %d chars: ", ret);
			for (i = 0; i < ret; i++) fprintf(stderr, "%c [%02x] ", data.raw[i], data.raw[i]);
		}
	}
	DEBUG fprintf(stderr, "After FD=%d fdset=%x ", fd, readfd);
}

/**************/
/* SENDSERIAL */
/**************/
int sendSerial(int fd, unsigned char data) {
	// Send a single byte.  Return 1 for a logged failure
	int retries = SERIALNUMRETRIES;
	int written;
	int newfd;
#ifdef DEBUGCOMMS
	fprintf(DEBUGFP, "Comm 0x%02x(%d) ", data, data);
	return 0;
#endif
	
	DEBUG2 fprintf(stderr, "%02x ", data);
	while ((written = write(fd, &data, 1)) < 1) {
        fprintf(stderr, "Serial wrote %d bytes errno = %d", written, errno);
		sprintf(buffer, "INFO " PROGNAME " SendSerial: Failed to write data: %s", strerror(errno));
		logmsg(INFO, buffer);
		close(fd);
		newfd = openSerial(serialName, BAUD, 0, CS8, 1);
		if (newfd < 0) {
			sprintf(buffer, "WARN " PROGNAME " SendSerial: Error reopening serial/port: %s ", strerror(errno));
			logmsg(WARN, buffer);
		}
		if (newfd != fd) {
			sprintf(buffer, "WARN " PROGNAME " SendSerial: Problem reopening socket - was %d now %d", fd, newfd);
			logmsg(WARN, buffer);
			return 1;
		}
		if (--retries == 0) {
			sprintf(buffer, "WARN " PROGNAME " %d SendSerial: too many retries", controllernum);
			logmsg(WARN, buffer);
			return 1;
		}
		DEBUG fprintf(stderr, "SendSerial retry pausing %d ... ", SERIALRETRYDELAY);
		usleep(SERIALRETRYDELAY);
	}
	return 0;       // ok
}

/***********/
/* CATCHER */
/***********/
void catcher(int sig) {
// Signal catcher
// Possibly, more than one process could have died.  Unlikely but possible
// In fact seems to be rather common - due to statusupdate.
// Improved thanks to APUE to discriminate between different types of exit 
// or indeed cause of SIGCHLD
	char buf[200];
	switch(sig) {
	case SIGPIPE:
		sprintf(buf, "INFO " PROGNAME " %d Caught SIGPIPE - ignoring", controllernum);
		logmsg(INFO, buf);
		break;
	default:
		sprintf(buf, "WARN " PROGNAME " %d Caught Signal %d", controllernum, sig);
		logmsg(WARN, buf);
	}
}
