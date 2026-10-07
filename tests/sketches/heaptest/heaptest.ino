// Free heap right at the start, after WiFi off and after a big allocation, on the serial port
// (115200): "HEAP name bytes". Device and emulator must agree: a program whose buffers just
// fit on the ESPboy must find the same room in the emulator.
#include <Arduino.h>
#include <ESP8266WiFi.h>

void setup()
{
    Serial.begin(115200);
    Serial.printf("\nHEAP start %u\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("HEAP maxblock %u\n", (unsigned)ESP.getMaxFreeBlockSize());
    WiFi.mode(WIFI_OFF);
    delay(100);
    Serial.printf("HEAP wifioff %u\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("HEAP wifioff_maxblock %u\n", (unsigned)ESP.getMaxFreeBlockSize());
    delay(1000);
    Serial.printf("HEAP later %u\n", (unsigned)ESP.getFreeHeap());
    Serial.println("HEAP done");
}

void loop()
{
    delay(1000);
}
