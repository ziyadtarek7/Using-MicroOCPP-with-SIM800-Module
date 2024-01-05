#include <Arduino.h>
#define SerialMon Serial
#define SerialAT Serial2
#include <MicroOcpp.h>
#include <WiFi.h>

#define STASSID "SSID"
#define STAPSK  "PASSWORD"

#define DEF_OCPP_HOST "HOST IP"
#define DEF_OCPP_PORT 8080
#define DEF_OCPP_URL "URL"
#define DEF_OCPP_ID "ID"

#if !defined(TINY_GSM_RX_BUFFER)
#define TINY_GSM_RX_BUFFER 1024
#endif

#define TINY_GSM_DEBUG SerialMon

#define TINY_GSM_USE_GPRS true
#define TINY_GSM_USE_WIFI false

const char apn[]      = "apn";
const char gprsUser[] = "";
const char gprsPass[] = "";

#define MODEM_RST 5

TinyGsm        modem(SerialAT);


void setup() {
  Serial.begin(115200);
  SerialAT.begin(9600);

  SerialMon.println("Initializing modem...");
  //  modem.restart();
  modem.init();
  String modemInfo = modem.getModemInfo();
  SerialMon.print("Modem Info: ");
  SerialMon.println(modemInfo);

  SerialMon.print(F("Connecting to "));
  SerialMon.print(apn);
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println(" fail");
    delay(10000);
    return;
  }
  SerialMon.println(" success");

  if (modem.isGprsConnected()) {
    SerialMon.println("GPRS connected");
  }

  /*
     Initialize the OCPP library
  */
  mocpp_initialize(DEF_OCPP_URL, DEF_OCPP_ID, "a", "a");

}

void loop() {
  mocpp_loop();
}
