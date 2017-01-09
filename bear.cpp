#include "SdFat/SdFat.h"


// TODO
// перенести функции по классам
// переменные организовать в структуры
// часовой пояс сохранять в EEPROM

SYSTEM_THREAD(ENABLED);
SYSTEM_MODE(SEMI_AUTOMATIC);

#define PGAMP D1
#define VBAT A0
#define PAIR A7
#define PGLED D7
#define DACR DAC1
#define DACL A3

#define USB Serial
#define BLE Serial1

#define NAME_LEN 20

#define MIDDLE_DAC 3102

// NETWORK
#define TCP_PACKET_LENGTH 256
TCPClient client;
long int fileReceivedLength = 0;
bool fileDownloaded = false;
int packet = 0;
bool tcpAtWork = false;

// FILE SYSTEM
// SCK => D4, MISO => D3, MOSI => D2, SS => D1
#define SPI_CONFIGURATION 1
SdFat sd(1);
File myFile;
File downloadingFile;
const uint8_t chipSelect = D5;

// PLAYING SOUNDS
#define sampleLen 4000
int playPeriod_us = 120;
int timeline = 0;
uint8_t sound[sampleLen];
bool tellStory = false;
bool storyPaused = true;
bool mustSyncStory = false;
char storyName[NAME_LEN];

// DOWNLOADING FILE
struct SyncFile {
    char name[NAME_LEN];
    uint8_t count; // parts amount for intaractive story
    uint8_t doneCount; // downloaded amount of parts
} storyToSync;

// ALARM
struct Alarm {
    uint8_t days = 0; // bits: 0 — mon, 1 — tue, … 6 — sun
    uint8_t time[2] = {0, 0}; // [0] — hours, [1] — minutes
    uint8_t status = 0;
	bool light = false;
    bool vibro = false;
    bool sound = false;
    bool isPlayingNow = false;
} alarm;

bool needWiFi = false;

// MAIN PROGRAM
void setup() {
    
    wifiOff();
    
    alarm.time[0] = EEPROM.read(0);
    alarm.time[1] = EEPROM.read(1);
    alarm.days = EEPROM.read(2);
    alarm.status = EEPROM.read(3);
    
    Time.zone(+3);
	
    pinMode(DACL, OUTPUT);
    pinMode(DACR, OUTPUT);
    
    pinMode(PGAMP, OUTPUT);
    pinMode(VBAT, INPUT);
    pinMode(PAIR, INPUT);
    pinMode(PGLED, OUTPUT);
    
    /// BLE.begin(4800);
    /// USB.begin(4800);
    
    BLE.begin(9600);
    USB.begin(9600);
        
    if (!sd.begin(chipSelect, SPI_HALF_SPEED)) {
        // sd.initErrorHalt();
    }
    
}

void loop() {
    
    digitalWrite(PGLED, digitalRead(PAIR));

    if (tellStory) {
        readStory();
    }
    
    if (mustSyncStory) {
        if (Particle.connected()) {
            mustSyncStory = false;
            syncStory(storyToSync.name, storyToSync.doneCount + 1);
        }
    }
    
    // testPing();
    
    if (tcpAtWork) {
        checkTCP();
    }

}

void testPing() {
    delay(500);
    digitalWrite(D7, HIGH);
    USB.println("ping");
    delay(500);
    USB.println("ping");
    digitalWrite(D7, LOW);
}

void serialEvent() {
    while (USB.available()) {
        char c = USB.read();
        USB.print(c);
        parseCMD(c);
    }
}

void serialEvent1() {   
    while (BLE.available()) { 
        char c = BLE.read();
        USB.print(c);
        parseCMD(c);
    }
}

char parseCMD (char c) {
    switch (c) {
        case 's': startStopStory(); BLE.println("story"); break;
        case 'p': pauseStory(); BLE.println("pause"); break;
        case 'w': toggleWifi(); BLE.println("wifi"); break;
        case 'c': setSSID(); BLE.println("wifi"); break;
        // case 'U': broadcast(); break;
        case 'l': scanDir(); BLE.println("list"); break;
        case 't': alarmSet(); BLE.println("alarm"); break;
        case 'd': date(); BLE.println("date"); break;
        case 'h': heartBeat(); BLE.println("poll"); break;
        case 'r': removeFile(); BLE.println("remove"); break;
        case 'y': {
            memset(storyToSync.name, 0, NAME_LEN);
			BLE.readBytesUntil('\n', storyToSync.name, NAME_LEN);
			storyToSync.count = BLE.parseInt();
			if (storyToSync.count <= 1) {
			    storyToSync.count = 1;
			}
			storyToSync.doneCount = 0;
			
			USB.println("sync:");
			USB.printlnf("\tname: %s", storyToSync.name);
			USB.printlnf("\tparts: %d", storyToSync.count);
			
            if (Particle.connected() == false) {
                USB.println("wifiOn");
                wifiOn();
            }
            mustSyncStory = true;
            BLE.println("download");
        } break;
        default: break;
    }
    return c;
}

void setSSID() {
    
    WiFi.on();
    
    char ssid[NAME_LEN];
    char password[NAME_LEN];
    memset(ssid, 0, NAME_LEN);
    memset(password, 0, NAME_LEN);
	BLE.readBytesUntil('\n', ssid, NAME_LEN);
	BLE.readBytesUntil('\n', password, NAME_LEN);
    
    WiFi.setCredentials(ssid, password);
    
    WiFi.off();
    
    BLE.println("ok");
}

void readStory () {
    
    if (storyPaused) return;
    
    int hasRead = myFile.read(sound, sampleLen);
	if (hasRead > 0) {
        timeline++;
		for (int i = 0; i < hasRead; i++) {
			analogWrite(DACR, 2*sound[i] + MIDDLE_DAC);
			analogWrite(DACL, 2*sound[i] + MIDDLE_DAC);
			delayMicroseconds(playPeriod_us);
		}
	} else {
	    USB.println("THE END");
	    stopStory();
	}
}

void heartBeat() {
    
    waitForN();
    
    if (tcpAtWork && fileReceivedLength > 0) {
        if (storyToSync.count > 1) { 
            BLE.printlnf("d%s_%d:%d", storyToSync.name, storyToSync.doneCount+1, fileReceivedLength);
            USB.printlnf("d%s_%d:%d", storyToSync.name, storyToSync.doneCount+1, fileReceivedLength);
        } else { 
            BLE.printlnf("d%s:%d", storyToSync.name, fileReceivedLength);
            USB.printlnf("d%s:%d", storyToSync.name, fileReceivedLength);
        }
    }
    if (fileDownloaded) {
        BLE.printlnf("f%s", storyToSync.name);
        USB.printlnf("f%s", storyToSync.name);
    }
    if (tellStory) {
        BLE.printlnf("t%d", timeline);
        USB.printlnf("t%d", timeline);
        
        if (storyPaused) {
            USB.printlnf("p%s", storyName);
            BLE.printlnf("p%s", storyName);
        } else {
            USB.printlnf("s%s", storyName);
            BLE.printlnf("s%s", storyName);
        }
    }
    if (Particle.connected()) {
        USB.println("Particle.connected");
        BLE.println("w4");
    } else {
        if (WiFi.ready()) {
            USB.println("WiFi.ready");
            BLE.println("w3");
        } else {
            if (WiFi.connecting()) {
                USB.println("WiFi.connecting");
                BLE.println("w1");
            }
        }
    }
    
    
    // if (tellStory && storyPaused) {
    //     BLE.print("r");
    //     // BLE.println(fileReceivedLength);
    // }
    
    USB.printlnf("v%f", analogRead(VBAT) * 4.2 / 4095.0);

    if (alarm.isPlayingNow) {
        BLE.println("aon");
        USB.println("aon");
    }
}

void alarmSet () {
    
/* unit8_t alarmDays = 0; // bits: 0 — mon, 1 — tue, … 6 — sun
unit8_t alarmTime[2] = {0, 0}; // [0] — hours, [1] — minutes
bool alarmLight = false;
bool alarmVibro = false;
bool alarmSound = false; */
    char buffer[6];
    memset(buffer, 0, 6);
    while (!BLE.available());
    int c = BLE.peek();
    USB.println(c, HEX);
    if (c == 0x67) {
        BLE.read();
        waitForN();
        USB.println("Get alarm time");
    } else {
    	if (BLE.readBytes(buffer, 5) != 5) {
    	    USB.println("alarm:");
    	    for (int i = 0; i < 5; i++) {
    	        USB.println((uint8_t)buffer[i], HEX);
    	    }
    		return;
    	}
    	alarm.time[0] = buffer[0];
    	alarm.time[1] = buffer[1];
    	alarm.days = buffer[2];
    	alarm.status = buffer[3];
    	if (buffer[4] != '\n') {
    	    USB.print("last byte not \\n:");
    	    USB.println(buffer[4]);
    	    return;
    	}
    	
        EEPROM.write(0, alarm.time[0]);
        EEPROM.write(1, alarm.time[1]);
        EEPROM.write(2, alarm.days);
        EEPROM.write(3, alarm.status);
    }
	
	/*int h = BLE.parseInt();
	
	if (h)
	
	alarm.time[0] = BLE.parseInt();
	alarm.time[1] = BLE.parseInt();
	
	char buffer[8];
	memset(buffer, 0, 8);
	BLE.readBytesUntil('\n', buffer, 8);
	for (int i = 0; i < 8; i++) {
	    if (buffer[i] == '1') {
	        alarm.days += 1 << i;
	    }
	}
	
	memset(buffer, 0, 8);
	BLE.readBytesUntil('\n', buffer, 5);
	
	alarm.status += (buffer[0] == '1') ? 1 << 0 : 0;
	alarm.status += (buffer[1] == '1') ? 1 << 1 : 0;
	alarm.status += (buffer[2] == '1') ? 1 << 2 : 0;
	alarm.status += (buffer[3] == '1') ? 1 << 3 : 0;*/
	
	BLE.write(alarm.time[0]);
	BLE.print('|');
	BLE.write(alarm.time[1]);
	BLE.print('|');
	BLE.write(alarm.days);
	BLE.print('|');
	BLE.write(alarm.status);
	BLE.println();
		
	// USB.print(buffer);
}

void date () {
    
    unsigned long timestamp = BLE.parseInt();
    
    USB.println(timestamp);
    int zone = BLE.parseInt();
    
    Time.zone(zone);
    USB.println(zone);

    Time.setTime(timestamp);
    USB.println(Time.timeStr());
}

void startStopStory () {
    
    /* bool foundDelimeter = false;
    int length = 0;
    while (!foundDelimeter) {
        if (BLE.available()) {         ///////////////////
            char c = BLE.read();       ///////////////////
            
            USB.print(c);
            
            if (c != '\n') {
                storyName[length++] = c;
            } else {
                storyName[length] = 0;
                foundDelimeter = true;
            }
        }
    }*/
    
    if (BLE.readBytesUntil('\n', storyName, NAME_LEN) == 0) {
    //if (length == 0) {
        stopStory();
    } else {
        startStory();
    }
}

void stopStory () {
    
    if (!tellStory) return;
    
    analogWrite(DACR, MIDDLE_DAC);
    analogWrite(DACL, MIDDLE_DAC);    
    delay(100);
    digitalWrite(PGAMP, LOW);
    
    tellStory = false;
    if (myFile.isOpen()) {
        myFile.close();
    }
    
    USB.println("stop");
    BLE.println("stop");
}

void startStory () {
    
    if (tellStory) {
        stopStory();
    }
    
    storyPaused = false;
    tellStory = true;
    
    digitalWrite(PGAMP, HIGH);
    
    if (myFile.isOpen()) {
        myFile.close();
    }
    
    char filename[NAME_LEN + 4];
    memset(filename, 0, NAME_LEN + 4);
    sprintf(filename, "%s.raw", storyName);
    
    if (!myFile.open(filename, O_READ)) {
        USB.println("error open file for read");
        stopStory();
        //Serial.println(sd.error());
        // sd.errorHalt("opening tale.wav for read failed");
    }
    
    timeline = 0;
    
    USB.println("start");
    BLE.println("start");
    
}

void removeFile () {

    char name[NAME_LEN];
    memset(name, 0, NAME_LEN);
	BLE.readBytesUntil('\n', name, NAME_LEN);
	uint8_t count = BLE.parseInt();
	if (count <= 1) {
	    count = 1;
	}
	
    USB.println("remove:");
    USB.printlnf("\tname: %s", name);
    USB.printlnf("\tparts: %d", count);
	
	char filename[NAME_LEN + 4];
	memset(filename, 0, NAME_LEN + 4);
	
	for (uint8_t i = 1; i <= count; i++) {
        if (count == 1) {
            sprintf(filename, "%s.raw", name);
        } else {
            sprintf(filename, "%s_%d.raw", name, i);
        }
        
        if (myFile.isOpen()) {
            myFile.close();
        }
        
        if (!sd.remove(filename)) {
            USB.printlnf("failed remove: %s", filename);
        }
	}
    
}

void checkTCP () {
    if (client.available()) {
        uint8_t buffer[TCP_PACKET_LENGTH] = {};
        int bytesRead = client.read(buffer, TCP_PACKET_LENGTH);
        
        if (downloadingFile.write(buffer, bytesRead) < bytesRead) {
            USB.println("data write error");
        }
        
        fileReceivedLength += bytesRead;
        packet += bytesRead;
        if (fileReceivedLength < 500) {
            USB.write(buffer, bytesRead);
        }
        if (packet > 100000) {
            USB.println(fileReceivedLength);
            packet = 0;
        }
    }

    if (!client.connected()) {
        USB.println();
        USB.print("Total: ");
        USB.println(fileReceivedLength);
        USB.println("disconnecting.");
        client.stop();
        tcpAtWork = false;
        
        storyToSync.doneCount++;
        if (storyToSync.count == storyToSync.doneCount) {
            fileDownloaded = true; // all files downloaded
            wifiOff();
        } else {
            mustSyncStory = true;
        }
        
        if (downloadingFile.isOpen()) {
            if (!downloadingFile.sync()) {
                USB.println("sync() failed");
            }
            downloadingFile.close();
        }
    }
}

void syncStory (char* name, int count) {
    
    // http://storage.googleapis.com/hardteddy_stories/1.raw
    
  // if (client.connect("hardteddy.ru", 80)) {
  if (client.connect("storage.googleapis.com", 80)) {
      
    stopStory();
      
    USB.println("connected");
    // BLE.println("connected");
    
    // client.print("GET /story/");
    client.print("GET /hardteddy_stories/");
    
    char filename[NAME_LEN + 4];
    memset(filename, 0, NAME_LEN + 4);
    if (storyToSync.count > 1) {
        sprintf(filename, "%s_%d.raw", name, count);
    } else {
        sprintf(filename, "%s.raw", name);
    }
    
    client.print(filename);
    client.println(" HTTP/1.0");
    // client.println("Host: hardteddy.ru");
    client.println("Host: storage.googleapis.com");
    client.println("Content-Length: 0");
    client.println();
    
    packet = 0;
    fileReceivedLength = 0;
    tcpAtWork = true;
    
    if (!downloadingFile.open(filename, O_CREAT | O_WRITE)) {
        USB.print(filename);
        USB.println(" create failed");
        // sd.errorHalt("create response.txt failed");
    }
    
  } else {
    USB.println("connection failed");
    // BLE.println("connection failed");
  }
}

void scanDir() {
    waitForN();
    fileDownloaded = false;
    sd.ls(&BLE, LS_R);
    sd.ls(&USB, LS_R);
}

void pauseStory () {
    
    waitForN();

    storyPaused = !storyPaused;
    if (storyPaused) {
        analogWrite(DAC1, 0);
        pinMode(DAC1, INPUT);
        digitalWrite(PGAMP, LOW);
    } else {
        pinMode(DAC1, OUTPUT);
        digitalWrite(PGAMP, HIGH);
    }
    
    USB.println("pause");
}

void toggleWifi () {
    
    waitForN();
    
    if (Particle.connected() || WiFi.connecting() || WiFi.ready()) {
        wifiOff();
        BLE.println("off");
        USB.println("WiFi off");

    } else {
        wifiOn();
        BLE.println("on");
        USB.println("WiFi on");
    }
};
void wifiOn () {
    RGB.control(false);
    needWiFi = true;
    Particle.connect();
};
void wifiOff () {
    RGB.control(true);
    RGB.color(0, 0, 0); // turnOff RGB led
    needWiFi = false;
    WiFi.off();
};

void waitForN () {
    while (true) {
        if (BLE.available()) {
            BLE.read(); // expect '\n'
            break;
        }
        if (USB.available()) {
            USB.read(); // expect '\n'
            break;
        }
    }
}