#include <SPI.h>
#include <SD.h>
// Date and time functions using a DS3231 RTC connected via I2C and Wire lib
#include "RTClib.h"

////////////////////////////////////////////////////////////////////////////
// modify these values to control behavior
////////////////////////////////////////////////////////////////////////////

// set to true to enable hibernation behavior
const boolean hibernate = true;
// if we haven't recieved a data packet in this many mS, we'll sleep
const int lastDataTimeThreshold = 1500;
// how long to sleep (keep in mind how long a vehicle will spend in view of
// your radar and set this value comfortably lower than this)
const int sleepTime = 400;
//const int sleepTime = 750;
// at minimum, how long should the radar stay on after waking up from sleep
const int minOnTime = 750;
//longest time a car could be in view of sensor before we record a datapoint
const int timeStartThreshold = 15000;
//log and reset if we haven't had a speed reading in this long
const int timeLastThreshold = 600;
// magnitude change threshold (e.g., trailing car will cause step drop in inbound magnitude when picked up)
const int magThreshold = 200;
// log ALL datapoints?
const boolean logAll = true;

/////////////////////////////////////////////////////////////////////////////
// do not modify below this line
/////////////////////////////////////////////////////////////////////////////
RTC_DS3231 rtc;
//RTCZero rtc;

const int chipSelect = SDCARD_SS_PIN;
File dataFile;
float currentSpeed;
float currentMag;

float inboundSpeedMax;
char inboundSpeedMaxChar[8];
int inboundTimeStart;
int inboundTimeLast;
float inboundMag;
float inboundMagLast;

float outboundSpeedMax;
char outboundSpeedMaxChar[8];
int outboundTimeStart;
int outboundTimeLast;
float outboundMag;
float outboundMagLast;

char radarData[256];
char speed[8];
char mag[8];
char logMessage[128];
char * pch;
boolean usbSerial;
int timeLastFlush;
int lastDataTime;
int lastSleepTime;
boolean asleep;

int now;
DateTime timestamp;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  // Open serial communications and wait for port to open:
  Serial.begin(9600);
  int serialInit = millis();
  while (!Serial && (millis()-serialInit) < 5000) {
    ; // wait for 5 seconds for serial port to connect. Needed for native USB port only
  }
  if (Serial) {
    usbSerial = true;
  } else {
    usbSerial = false;
  }
  Serial1.begin(19200);
  
  if(usbSerial) Serial.println("Initializing SD card...");

  // see if the card is present and can be initialized:
  if (!SD.begin(chipSelect)) {
    if(usbSerial) Serial.println("Card failed, or not present");
    // don't do anything more:
    while (1);
  }
  if(usbSerial) Serial.println("card initialized.");
  
  rtc.begin();
  
  if (! rtc.begin()) {
    if(usbSerial) Serial.println("Couldn't find RTC");
  }
  if (rtc.lostPower()) {
    if(usbSerial) Serial.println("RTC lost power, let's set the time!");
    // When time needs to be set on a new device, or after a power loss, the
    // following line sets the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
  }
  //rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  rtc.disable32K();

  // setup all our radar options
  // radar sign mode
  Serial1.println("On");
  // mph units
  Serial1.println("US");
  // JSON mode
  Serial1.println("OJ");
  // minimum speed
  Serial1.println("R>10");
  // sample rate to 20,000 for max speed 139.1mph
  Serial1.println("S2");
  // turn on magnitude reports
  Serial1.println("OM");
  // turn on hibernate mode
  //Serial1.println("Z-");
  Serial1.println("PA");

  // open the file. note that only one file can be open at a time,
  // so you have to close this one before opening another.
  //String timestamp =
  int logIndex = 1;
  char logIndexChar[4];
  char logFilename[10];
  memset(logFilename, 0, sizeof(logFilename));
  memset(logIndexChar, 0, sizeof(logIndexChar));
  itoa(logIndex, logIndexChar, 10);
  strcat(logFilename, logIndexChar);
  strcat(logFilename, ".txt");

  while (SD.exists(logFilename)) {
    logIndex++;
    memset(logFilename, 0, sizeof(logFilename));
    memset(logIndexChar, 0, sizeof(logIndexChar));
    itoa(logIndex, logIndexChar, 10);
    strcat(logFilename, logIndexChar);
    strcat(logFilename, ".txt");
  }
  dataFile = SD.open(logFilename, FILE_WRITE);

  if (dataFile) {
    if(usbSerial) {
      Serial.print("Successfully opened log file ");
      Serial.println(logFilename);
    }
    dataFile.println("Radar Data starting...");
    dataFile.print(rtc.now().timestamp(DateTime::TIMESTAMP_DATE));
    dataFile.print(",");
    dataFile.print(rtc.now().timestamp(DateTime::TIMESTAMP_TIME));
    dataFile.println("");
    dataFile.println("dateTime,timeSinceStart,timeSinceLast,magnitude,lastMagnitude,speed,type");
  } else {
    if(usbSerial) {
      Serial.print("Error opening log file ");
      Serial.println(logFilename);
    }
  }
  // wait 10 seconds with radar LED ON to confirm operation, then turn off to save power and proceed to data collection
  Serial1.println("OL");
  delay(10000);
  Serial1.println("Ol");

  timeLastFlush = 0;
  digitalWrite(LED_BUILTIN, LOW);   //turn off LED by default
  lastDataTime = millis();
  lastSleepTime = millis();
  asleep = false;
}

void loop() {
  now = millis();
  if (Serial1.available()) {       // If anything comes in Serial1 (pins 0 & 1)
    int bytes = Serial1.readBytesUntil('\n', radarData, 256);
    if (bytes > 0)  {
      if(usbSerial) Serial.println(radarData);
      pch = strstr(radarData, "speed");
      if (pch != NULL) {
        //if(usbSerial) Serial.println(pch);
        lastDataTime = now;
        memcpy(speed, &pch[10], strlen(pch) - 13);
        memcpy(mag, &radarData[16], bytes - strlen(pch) - 20);
        currentSpeed = atof(speed);
        currentMag = atof(mag);
        if(usbSerial) Serial.print(speed);
        if(usbSerial) Serial.print("  ");
        if(usbSerial) Serial.println(mag);
        if (currentSpeed < 0 ) {
          if (outboundSpeedMax == 0)  {
            outboundTimeStart = now;
          }
         if (logAll) {
            timestamp = rtc.now();
            dataFile.print(timestamp.timestamp(DateTime::TIMESTAMP_DATE));
            dataFile.print(" ");
            dataFile.print(timestamp.timestamp(DateTime::TIMESTAMP_TIME));
            dataFile.print(",");
            dataFile.print((now - outboundTimeStart));
            dataFile.print(",");
            dataFile.print((now - outboundTimeLast));
            dataFile.print(",");
            dataFile.print(currentMag);
            dataFile.print(",");
            dataFile.print(outboundMag);
            dataFile.print(",");
            dataFile.print(speed);
            dataFile.print(",");
            dataFile.println("frame");
          }

          outboundTimeLast = now;

          if (currentSpeed < outboundSpeedMax) {
            outboundSpeedMax = currentSpeed;
            strcpy(outboundSpeedMaxChar, speed);
          }
          outboundMagLast = outboundMag;
          outboundMag = currentMag;
        }
        if (currentSpeed > 0 ) {
          if (inboundSpeedMax == 0)  {
            inboundTimeStart = now;
          }

          if (logAll) {
            timestamp = rtc.now();
            dataFile.print(timestamp.timestamp(DateTime::TIMESTAMP_DATE));
            dataFile.print(" ");
            dataFile.print(timestamp.timestamp(DateTime::TIMESTAMP_TIME));
            dataFile.print(",");
            dataFile.print((now - inboundTimeStart));
            dataFile.print(",");
            dataFile.print((now - inboundTimeLast));
            dataFile.print(",");
            dataFile.print(currentMag);
            dataFile.print(",");
            dataFile.print(inboundMag);
            dataFile.print(",");
            dataFile.print(speed);
            dataFile.print(",");
            dataFile.println("frame");
          }

          inboundTimeLast = now;

          if (currentSpeed > inboundSpeedMax) {
            inboundSpeedMax = currentSpeed;
            strcpy(inboundSpeedMaxChar, speed);
          }
          inboundMagLast = inboundMag;
          inboundMag = currentMag;
        }
        memset(speed, 0, sizeof(speed));
        memset(mag, 0, sizeof(mag));
        memset(pch, 0, sizeof(pch));
      }
      memset(radarData, 0, sizeof(radarData));
    }
  }

  // check if either inbound or outbound objects have timed out
  // if they have, write them to log file and reset values
  if (((now - outboundTimeStart) > timeStartThreshold || (now - outboundTimeLast) > timeLastThreshold) && outboundSpeedMax < 0) {
    timestamp = rtc.now();
    dataFile.print(timestamp.timestamp(DateTime::TIMESTAMP_DATE));
    dataFile.print(" ");
    dataFile.print(timestamp.timestamp(DateTime::TIMESTAMP_TIME));
    dataFile.print(",");
    dataFile.print((now - outboundTimeStart));
    dataFile.print(",");
    dataFile.print((now - outboundTimeLast));
    dataFile.print(",");
    dataFile.print(outboundMag);
    dataFile.print(",");
    dataFile.print(outboundMagLast);
    dataFile.print(",");
    dataFile.print(outboundSpeedMaxChar);
    dataFile.print(",");
    dataFile.println("vehicle");
    if (usbSerial) {
      Serial.print(timestamp.timestamp(DateTime::TIMESTAMP_DATE));
      Serial.print(" ");
      Serial.print(timestamp.timestamp(DateTime::TIMESTAMP_TIME));
      Serial.print(",");
      Serial.print((now - outboundTimeStart));
      Serial.print(",");
      Serial.print((now - outboundTimeLast));
      Serial.print(",");
      Serial.print(outboundMag);
      Serial.print(",");
      Serial.print(outboundMagLast);
      Serial.print(",");
      Serial.print(outboundSpeedMaxChar);
      Serial.print(",");
      Serial.println("vehicle");
    }
    outboundSpeedMax = 0;
    memset(outboundSpeedMaxChar, 0, sizeof(outboundSpeedMaxChar));
  }
  if (((now - inboundTimeStart) > timeStartThreshold || (now - inboundTimeLast) > timeLastThreshold) && inboundSpeedMax > 0) {
    timestamp = rtc.now();
    dataFile.print(timestamp.timestamp(DateTime::TIMESTAMP_DATE));
    dataFile.print(" ");
    dataFile.print(timestamp.timestamp(DateTime::TIMESTAMP_TIME));
    dataFile.print(",");
    dataFile.print((now - inboundTimeStart));
    dataFile.print(",");
    dataFile.print((now - inboundTimeLast));
    dataFile.print(",");
    dataFile.print(inboundMag);
    dataFile.print(",");
    dataFile.print(inboundMagLast);
    dataFile.print(",");
    dataFile.print(inboundSpeedMaxChar);
    dataFile.print(",");
    dataFile.println("vehicle");
    if (usbSerial) {
      Serial.print(timestamp.timestamp(DateTime::TIMESTAMP_DATE));
      Serial.print(" ");
      Serial.print(timestamp.timestamp(DateTime::TIMESTAMP_TIME));
      Serial.print(",");
      Serial.print((now - inboundTimeStart));
      Serial.print(",");
      Serial.print((now - inboundTimeLast));
      Serial.print(",");
      Serial.print(inboundMag);
      Serial.print(",");
      Serial.print(inboundMagLast);
      Serial.print(",");
      Serial.print(inboundSpeedMaxChar);
      Serial.print(",");
      Serial.println("vehicle");
    }
    inboundSpeedMax = 0;
    memset(inboundSpeedMaxChar, 0, sizeof(inboundSpeedMaxChar));
  }

  // flush SD card writes every 10 seconds
  if (now - timeLastFlush > 10000) {
    digitalWrite(LED_BUILTIN, HIGH);
    if(usbSerial) Serial.println("Flushing SD buffer...");
    dataFile.flush();
    if(usbSerial) Serial.println("Done flushing SD buffer.");
    timeLastFlush = now;
    digitalWrite(LED_BUILTIN, LOW);
  }

  //sleep if we haven't recieved data in lastDataTimeThreshold and we've been awake for at least minOnTime, time to sleep
  if (hibernate && (now - lastDataTime > lastDataTimeThreshold) && (now - lastSleepTime > (sleepTime+minOnTime)) && !asleep) {
    // put radar in low-power mode
    Serial1.println("PI");
    lastSleepTime = now;
    asleep = true;
  }
  // if we've been sleeping for more than sleepTime, time to wake up
  if (hibernate && (now - lastSleepTime > sleepTime) && asleep) {
    // put radar in active power mode
    Serial1.println("PA");
    asleep = false;
  }
  
}

