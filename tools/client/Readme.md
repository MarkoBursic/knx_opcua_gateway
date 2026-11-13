
Zagon skripte, izdela certifikat za clienta - uporabi samo če clinet nima svojega certificate maagerja.
Če ima client svoje certificate manager, uporabi tistega in uporabi v tools/trsust_client.sh
Če uoprabiš ta generator, moraš certifikat naložit v clienta in prav tako uporabiti:
bash tools/trust_client.sh tools/client/pki_client/client_cert.der 

V terminalu ji dodelite pravice za izvajanje in jo zaženite:
bash
chmod +x gen_client_cert.sh
./gen_client_cert.sh
Kodo uporabljajte previdno.

Kaj skripta naredi

• Brez odvečnih datotek: Začasno konfiguracijo OpenSSL ustvari v pomnilniku (/tmp) in jo po koncu samodejno izbriše.
• Oba formata hkrati: Ustvari tako .pem kot .der različico certifikata in ključa v mapi pki_client/.
• Popravljen keyUsage: Odstranjen je keyCertSign, da ne bo prišlo do zavrnitve s strani OPC UA sklada zaradi kombinacije z CA:FALSE.
Če želite, lahko skripto dopolnimo tako, da samodejno skopira client_cert.der v mapo pki/trusted/ vašega strežnika ali pa prilagodi veljavnost certifikata (npr. na 10 ali 45 let, kot ste imeli pri strežniku). Bi želeli katero od teh prilagoditev?
