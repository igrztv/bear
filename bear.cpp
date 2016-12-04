#include "SdFat/SdFat.h"


// TODO
// перенести функции по классам
// переменные организовать в структуры
// часовой пояс сохранять в EEPROM
// readUntil и readBLEUntil унифицировать

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

// NETWORK
#define TCP_PACKET_LENGTH 256
#define LOCAL_PORT 14567
UDP Udp;
TCPClient client;
long int fileReceivedLength = 0;
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
#define sampleLen 8000
int playPeriod_us = 120;
uint8_t sound[sampleLen];
bool tellStory = false;
bool storyPaused = true;
bool mustSyncStory = false;
char storyName[20];
char storyToSync[20];

// ALARM

struct Alarm {
    uint8_t alarmDays = 0; // bits: 0 — mon, 1 — tue, … 6 — sun
    uint8_t alarmTime[2] = {0, 0}; // [0] — hours, [1] — minutes
    bool alarmLight = false;
    bool alarmVibro = false;
    bool alarmSound = false;
    uint8_t alarmStatus = 0;
    bool alarmIsPlayingNow = false;
} alarm;

// MAIN PROGRAM
void setup() {
    
    wifiOff();
    
    alarmTime[0] = EEPROM.read(0);
    alarmTime[1] = EEPROM.read(1);
    alarmDays = EEPROM.read(2);
    alarmStatus = EEPROM.read(3);
    
    Time.zone(+3);
	
    pinMode(DACL, OUTPUT);
    pinMode(DACR, OUTPUT);
    
    pinMode(PGAMP, OUTPUT);
    pinMode(VBAT, INPUT);
    pinMode(PAIR, INPUT);
    pinMode(PGLED, OUTPUT);
    
    Udp.begin(LOCAL_PORT);
    
    // BLE.begin(4800);
    // USB.begin(4800);
    
    BLE.begin(9600);
    USB.begin(9600);
        
    if (!sd.begin(chipSelect, SPI_HALF_SPEED)) {
        sd.initErrorHalt();
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
            syncStory(storyToSync, 0);
        }
    }
    
    // testPing();
    
    if (Udp.parsePacket() > 0) {
        while(Udp.available() > 0) {
            char c = Udp.read();
            USB.print(c);
        }
        USB.println();
    }
    
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
        parseCMD(c);
        // Serial1.print(parseCMD(c));
    }
}

void serialEvent1() {   
    while (BLE.available()) { 
        char c = BLE.read();
        // parseCMD(c);
        USB.print(parseCMD(c));
    }
}

char parseCMD (char c) {
    switch (c) {
        case 's': startStopStory(); BLE.println("story"); break;
        case 'p': pauseStory(); BLE.println("pause"); break;
        case 'w': toggleWifi(); BLE.println("wifi"); break;
        case 'c': setSSID(); BLE.println("wifi"); break;
        case 'U': broadcast(); break;
        case 'l': scanDir(); BLE.println("list"); break;
        case 't': alarmSet(); BLE.println("alarm"); break;
        case 'd': date(); BLE.println("date"); break;
        case 'h': heartBeat(); BLE.println("poll"); break;
        case 'r': removeFile(); BLE.println("remove"); break;
        case 'y': {
            USB.println("sync:");
            readBLEUntil(storyToSync, 20, '\n');
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
    
    char ssid[20];
    char password[20];
    readBLEUntil(ssid, 20, '\n');
    readBLEUntil(password, 20, '\n');
    
    WiFi.setCredentials(ssid, password);
    
    WiFi.off();
    
    BLE.println("ok");
}

void readStory () {
    
    if (storyPaused) return;
    
    int hasRead = myFile.read(sound, sampleLen);
	if (hasRead > 0) {
		for (int i = 0; i < hasRead; i++) {
			analogWrite(DACR, sound[i] + 2048);
			analogWrite(DACL, sound[i] + 2048);
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
        BLE.print("d");
        BLE.println(fileReceivedLength);
        
        USB.print("d");
        USB.println(fileReceivedLength);
    }
    if (tellStory && !storyPaused) {
        BLE.print("s");
        BLE.println(storyName);
        
        USB.print("s");
        USB.println(storyName);
    }
    if (tellStory && storyPaused) {
        BLE.print("p");
        BLE.println(storyName);
        
        USB.print("p");
        USB.println(storyName);
    }
    // if (tellStory && storyPaused) {
    //     BLE.print("r");
    //     // BLE.println(fileReceivedLength);
    // }

    USB.print("v");
    USB.println(analogRead(VBAT) * 4.2 / 4095.0);

    if (alarmIsPlayingNow) {
        BLE.print("a");
        BLE.println("on");
        
        USB.print("a");
        USB.println("on");
    }
}

void alarmSet () {
    
/* unit8_t alarmDays = 0; // bits: 0 — mon, 1 — tue, … 6 — sun
unit8_t alarmTime[2] = {0, 0}; // [0] — hours, [1] — minutes
bool alarmLight = false;
bool alarmVibro = false;
bool alarmSound = false; */

    bool foundDelimeter = false;
    int i = 0;
    while (!foundDelimeter) {
        if (BLE.available()) {
            char c = BLE.read();
            USB.print(c);
            if (c != '\n') {
                if (i == 0) alarmTime[0] = c;
                if (i == 1) alarmTime[1] = c;
                if (i == 2) alarmDays = c;
                if (i == 3) alarmStatus = c;
                i++;
            } else {
                foundDelimeter = true;
            }
        }
    }

    if (i == 4) {
        EEPROM.write(0, alarmTime[0]);
        EEPROM.write(1, alarmTime[1]);
        EEPROM.write(2, alarmDays);
        EEPROM.write(3, alarmStatus);
    }
    
    BLE.write(alarmTime[0]);
    BLE.write(alarmTime[1]);
    BLE.write(alarmDays);
    BLE.write(alarmStatus);
    BLE.println();
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
    
    bool foundDelimeter = false;
    int length = 0;
    while (!foundDelimeter) {
        if (BLE.available()) {
            char c = BLE.read();
            
            USB.print(c);
            
            if (c != '\n') {
                storyName[length++] = c;
            } else {
                storyName[length] = 0;
                foundDelimeter = true;
            }
        }
    }
    
    if (length == 0) {
        stopStory();
    } else {
        startStory();
    }
}

void stopStory () {
    
    if (!tellStory) return;
    
    digitalWrite(PGAMP, LOW);
    analogWrite(DACR, 0);
    analogWrite(DACL, 0);
    
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
    
    char filename[20];
    sprintf(filename, "%s.raw", storyName);
    
    if (!myFile.open(filename, O_READ)) {
        USB.println("error open file for read");
        stopStory();
        //Serial.println(sd.error());
        // sd.errorHalt("opening tale.wav for read failed");
    }
    
    USB.println("start");
    BLE.println("start");
    
}

void removeFile () {

    char name[20];
    readBLEUntil(name, 20, '\n');
    
    char filename[20];
    sprintf(filename, "%s.raw", name);
    
    if (myFile.isOpen()) {
        myFile.close();
    }

    if (!sd.remove(filename)) {
        USB.println("failed remove file");
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
        
        if (downloadingFile.isOpen()) {
            if (!downloadingFile.sync()) {
                USB.println("sync() failed");
            }
            downloadingFile.close();
        }
    }
}

int readUntil(char* string, int maxLength, char delimeter) {
    bool foundDelimeter = false;
    int length = 0;
    while (!foundDelimeter) {
        if (USB.available()) {
            char c = USB.read();
            
            USB.print(c);
            
            if (c != delimeter) {
                string[length++] = c;
            } else {
                string[length] = 0;
                foundDelimeter = true;
                return length;
            }
            if (length == maxLength) {
                return maxLength;
            }
        }
    }
}

int readBLEUntil(char* string, int maxLength, char delimeter) {
    bool foundDelimeter = false;
    int length = 0;
    while (!foundDelimeter) {
        if (BLE.available()) {
            char c = BLE.read();
            
            USB.print(c);
            
            if (c != delimeter) {
                string[length++] = c;
            } else {
                string[length] = 0;
                foundDelimeter = true;
                return length;
            }
            if (length == maxLength) {
                return maxLength;
            }
        }
    }
}

void syncStory (char* name, int length) {
    
    // http://storage.googleapis.com/hardteddy_stories/1.raw
    
  // if (client.connect("hardteddy.ru", 80)) {
  if (client.connect("storage.googleapis.com", 80)) {
      
    stopStory();
      
    USB.println("connected");
    // BLE.println("connected");
    
    // client.print("GET /story/");
    client.print("GET /hardteddy_stories/");
    
    char filename[20];
    sprintf(filename, "%s.raw", name);
    
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
    
    /* stopStory();
    
    sd.vwd()->rewind();
    char name[20];
    while (myFile.openNext(sd.vwd(), O_READ)) {
        myFile.getName(name, 20);
        myFile.close();
        
        BLE.println(name);
        USB.println(name);
    }
    
    BLE.println("end");
    USB.println("end");*/
    
    waitForN();
    
    sd.ls(&BLE, LS_R);
    
    sd.ls(&USB, LS_R);
    USB.println("list");

}

void pauseStory () {
    
    while (!BLE.available());
    if (BLE.read() != '\n') return;

    storyPaused = !storyPaused;
    if (storyPaused) {
        analogWrite(DAC1, 0);
        pinMode(DAC1, INPUT);
    } else {
        pinMode(DAC1, OUTPUT);
    }
    
    USB.println("pause");
}

void toggleWifi () {
    
    waitForN();
    
    if (Particle.connected() || WiFi.connecting() || WiFi.ready()) {
        wifiOff();
        BLE.println("off");
    } else {
        wifiOn();
        BLE.println("on");
    }
};
void wifiOn () {
    Particle.connect();
};
void wifiOff () {
    WiFi.off();
};

void broadcast () {
    
    USB.println(WiFi.localIP());

    uint8_t address[] = {192, 168, 10, 92};
    IPAddress ipAddress( address );
    
    Udp.beginPacket(ipAddress, LOCAL_PORT);
    Udp.write("mishka");
    Udp.endPacket();
    
    USB.println("broadcast done");
    
}

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
