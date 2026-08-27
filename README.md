# KNX <-> OPC UA Dvosmerni Prehod (Gateway)

Prehod omogoča dvosmerno komunikacijo med pametnimi inštalacijami KNX (preko `knxd`) ter industrijskim protokolom OPC UA (preko knjižnice `open62541`).

## Lastnosti
- **Dvosmerno delovanje** s preprečevanjem povratnih zank.
- **Asinhrono delovanje** preko sistemskega klica `select()`.
- **Hierarhični Namespace**: Avtomatsko kreiranje map (npr. `knx.nadstropje1.soba1.lucvklop`).
- **Startup Scan**: Avtomatsko branje začetnih stanj ob zagonu (`GroupValue_Read`) s 50ms zamikom.
- **Auto-Reconnect**: Samodejno ponovno povezovanje na `knxd` vtičnico ob izpadih.
- **Testni način**: Omogočen anonimni dostop na portu 4840 za enostavno testiranje z *UaExpert*.

## Navodila za prevajanje
1. Namestite odvisnosti (npr. na Debian/Ubuntu):
   ```bash
   sudo apt-get install libopen62541-dev knxd
   ```
2. Prevedite projekt:
   ```bash
   make
   ```
3. Zaženite prehod:
   ```bash
   ./knx_opcua_gateway
   ```
