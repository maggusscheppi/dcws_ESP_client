# dcws
dcws - drone collision warning system<br>
This system provides a drone (or any other device) with an gps tracker, position viewer and warning system.<br>
It consists of<br>
LILYGO® TTGO T-Call V1.4 ESP32 Wireless Module SIM Antenna SIM Card SIM800L Module<br>
dcws client software<br>
dcws nodejs server<br>
dcws webbased viewer<br>

# dcws ESP client
## preconditions
You need a LILYGO® TTGO T-Call V1.4 ESP32 Wireless Module SIM Antenna SIM Card SIM800L Module<br>
e.g. available here: https://eckstein-shop.de/LILYGOC2AETTGOT-Call26PMUESP32WirelessModuleSIMAntennaSIMCardSIM800LModule <br>
a GPS module, I took the BN-220T<br>
e.g. available here: https://de.aliexpress.com/item/1005004208081387.html <br>
and an akku 3,7V <br>
e.g. with 1000mAh available here: https://de.aliexpress.com/item/1005003201823146.html <br>
(you have to check the correct connectors to the TTGO board) <br>
and a SIM card with some data volume (only ascii data will be transfered)<br>
e.g. Netzclub (200MB for free) : https://www.netzclub.net/ <br>
and the current ESP-IDF SDK<br>
https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html <br>
I have compiled and linked in 2020 with version 4.1 of the ESP-IDF <br>
