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

  /*
     Initialize Serial and WiFi
  */

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

  /*
     Integrate OCPP functionality. You can leave out the following part if your EVSE doesn't need it.
  */
  //  setEnergyMeterInput([]() {
  //    //take the energy register of the main electricity meter and return the value in watt-hours
  //    return 0.f;
  //  });
  //
  //  setSmartChargingCurrentOutput([](float limit) {
  //    //set the SAE J1772 Control Pilot value here
  //    Serial.printf("[main] Smart Charging allows maximum charge rate: %.0f\n", limit);
  //  });
  //
  //  setConnectorPluggedInput([]() {
  //    //return true if an EV is plugged to this EVSE
  //    return false;
  //  });

  //... see MicroOcpp.h for more settings
}

void loop() {

  /*
     Do all OCPP stuff (process WebSocket input, send recorded meter values to Central System, etc.)
  */
  mocpp_loop();
  //
  //  /*
  //     Energize EV plug if OCPP transaction is up and running
  //  */
  //  if (ocppPermitsCharge()) {
  //    //OCPP set up and transaction running. Energize the EV plug here
  //  } else {
  //    //No transaction running at the moment. De-energize EV plug
  //  }
  //
  //  /*
  //     Use NFC reader to start and stop transactions
  //  */
  //  if (/* RFID chip detected? */ false) {
  //    String idTag = "0123456789ABCD"; //e.g. idTag = RFID.readIdTag();
  //
  //    if (!getTransaction()) {
  //      //no transaction running or preparing. Begin a new transaction
  //      Serial.printf("[main] Begin Transaction with idTag %s\n", idTag.c_str());
  //
  //      /*
  //         Begin Transaction. The OCPP lib will prepare transaction by checking the Authorization
  //         and listen to the ConnectorPlugged Input. When the Authorization succeeds and an EV
  //         is plugged, the OCPP lib will send the StartTransaction
  //      */
  //      auto ret = beginTransaction(idTag.c_str());
  //
  //      if (ret) {
  //        Serial.println(F("[main] Transaction initiated. OCPP lib will send a StartTransaction when" \
  //                         "ConnectorPlugged Input becomes true and if the Authorization succeeds"));
  //      } else {
  //        Serial.println(F("[main] No transaction initiated"));
  //      }
  //
  //    } else {
  //      //Transaction already initiated. Check if to stop current Tx by RFID card
  //      if (idTag.equals(getTransactionIdTag())) {
  //        //card matches -> user can stop Tx
  //        Serial.println(F("[main] End transaction by RFID card"));
  //
  //        endTransaction(idTag.c_str());
  //      } else {
  //        Serial.println(F("[main] Cannot end transaction by RFID card (different card?)"));
  //      }
  //    }
  //  }
}
