#include "gateway.h"
#include "knx_eibd.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h> // Nujno za delovanje funkcije gettimeofday()

#include <open62541/server.h>
#include <open62541/server_config_default.h>

Config global_config;
UA_Server *server = NULL;
volatile UA_Boolean running = true;

// Kept for select() usage in the KNX thread below; the actual knxd
// connection handle lives inside knx_eibd.c (static, module-private).
int g_knx_fd = -1;

// Watchdog (section 3b of the KNX thread loop): warn when we keep sending
// reads but nothing answers for a long time - typical of a hung router.
#define WATCHDOG_MIN_READS    30   // at least this many of our reads without any answer...
#define WATCHDOG_SILENCE_SEC 300   // ...and at least this long since the last answer
#define WATCHDOG_REPEAT_SEC  600   // repeat the warning no more often than this

// Obremenitev vodila: razmaki med zaporednimi branji (da ostale naprave
// delujejo normalno) ter intervali ponovnega branja.
#define QUEUE_INTERVAL_MS        500   // zacetna vrsta: razmak med branji (prej 200 ms)
#define SMART_POLL_INTERVAL_MS  2000   // smart-poll: razmak med branji (prej 200 ms)
#define SMART_POLL_STALE_SEC    1800   // naprave, ki molcijo toliko casa, se osvezijo (30 min)
#define RETRY_UNANSWERED_SEC    7200   // naprave, ki po 3 poskusih niso odgovorile: ponovno cez 2 uri
#define POLL_ANSWER_TIMEOUT_SEC READ_ANSWER_WINDOW_SEC // po toliko s javimo, da KNX-POLL ni dobil odgovora

// Popoln ponovni pregled po ponovni povezavi samo, ce je izpad trajal vsaj
// toliko sekund. Pri nestabilni povezavi (ki pada vsakih nekaj sekund) bi
// sicer ~700 branj po vsakem padcu dodatno obremenjevalo ze tako ranljiv ruter.
#define RESCAN_MIN_OUTAGE_SEC 60

// SIGINT/SIGTERM (Ctrl+C, systemctl stop): leave UA_Server_run() cleanly.
static void handle_signal(int sig) {
    (void)sig;
    running = false;
}

// Ob izgubi povezave odgovorov na odprta smart-poll branja ni vec za pricakovati;
// brez tega bi po ponovni povezavi izpisali lazna "Brez odgovora".
static void clear_pending_polls(void) {
    for (size_t i = 0; i < global_config.dp_count; i++) {
        global_config.datapoints[i].read_sent_time = 0;
    }
}

// Marks every datapoint as "needs reading" and restarts the watchdog clock.
// Used at startup and after every successful reconnect, so values that
// changed while we were disconnected (knxd restart, PC suspend, router
// outage) are read again instead of staying stale.
static void reset_scan_state(time_t t) {
    for (size_t i = 0; i < global_config.dp_count; i++) {
        global_config.datapoints[i].is_initialized = false;
        global_config.datapoints[i].last_update_time = t;
        global_config.datapoints[i].last_read_sent = 0; // Števec poskusov branja
        global_config.datapoints[i].read_unanswered = false;
        global_config.datapoints[i].read_sent_time = 0;
    }
    g_last_response_time = t;
    g_reads_since_response = 0;
}

void* knx_thread_func(void *arg) {
    int knx_fd = init_knx_connection();
    g_knx_fd = knx_fd;
    bool knx_connected = (knx_fd >= 0);
    time_t disconnected_since = knx_connected ? 0 : time(NULL);
    bool need_full_scan = !knx_connected; // prva povezava je sele pred nami
    time_t last_reconnect_attempt = 0;
    time_t last_watchdog_warning = 0;
    time_t last_poll_check = 0;

    // Časovniki za upravljanje z vrstami prometa
    struct timeval last_queue_process_time;
    gettimeofday(&last_queue_process_time, NULL);

    struct timeval last_smart_poll_tick;
    gettimeofday(&last_smart_poll_tick, NULL);
    size_t smart_poll_cursor = 0;
    size_t current_queue_index = 0;
    bool queue_completely_finished = false; // Zastavica za konec začetnega scana

    // Počakamo sekundo, da se OPC UA strežnik v glavni niti popolnoma zažene
    sleep(1);

    if (knx_connected) {
        printf("[KNX] Zagon uspešen. Vse naprave dodane v začetno čakalno vrsto.\n");
        reset_scan_state(time(NULL));
    }

    printf("[SYSTEM] KNX nit uspešno zagnana. Začenjam optimizirano vrsto...\n");

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = 0;

        if (knx_connected) {
            FD_SET(knx_fd, &rfds);
            max_fd = knx_fd;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms odzivnost zanke

        int retval = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        time_t now = time(NULL);

        if (retval < 0) {
            perror("[SYSTEM] Napaka v select() zanki KNX");
            break;
        }

        // 0. Ali je pošiljalna povezava (druga nit - OPC UA zapisi, ali ta
        //    nit - queue/poll branja) sporočila napako? Brez tega bi ostala
        //    pokvarjena povezava neopažena, dokler naključno tudi sprejemna
        //    stran ne odpove.
        if (knx_connected && knx_eibd_check_and_clear_broken()) {
            printf("[WARN] KNX posiljalna povezava pokvarjena! Ponovno povezovanje...\n");
            clear_pending_polls();
            knx_eibd_close();
            g_knx_fd = -1;
            knx_connected = false;
            last_reconnect_attempt = now;
            if (!disconnected_since) disconnected_since = now;
        }

        // 1. Prestrezanje prometa (Spremembe ali odgovori iz vodila).
        //    Za razliko od prejšnje multicast implementacije, EIBGetGroup_Src
        //    (znotraj handle_knx_traffic) blokira le za preostanek sporočila,
        //    ki že prihaja, ne za poljuben prihodnji dogodek - zato kličemo
        //    natanko enkrat na select() prebujenje, brez drain zanke.
        if (knx_connected && FD_ISSET(knx_fd, &rfds)) {
            if (handle_knx_traffic(knx_fd, server) < 0) {
                printf("[WARN] KNX povezava izgubljena! Ponovno povezovanje...\n");
                clear_pending_polls();
                knx_eibd_close();
                g_knx_fd = -1;
                knx_connected = false;
                last_reconnect_attempt = now;
                if (!disconnected_since) disconnected_since = now;
            }
        }

        // 2. ASINHRONA ČAKALNA VRSTA: Branje začetnih stanj (ena naprava na QUEUE_INTERVAL_MS)
        if (knx_connected && !queue_completely_finished) {
            struct timeval tv_now;
            gettimeofday(&tv_now, NULL);

            long elapsed_ms = (tv_now.tv_sec - last_queue_process_time.tv_sec) * 1000 +
                               (tv_now.tv_usec - last_queue_process_time.tv_usec) / 1000;

            if (elapsed_ms >= QUEUE_INTERVAL_MS) { // Takt za začetni prehod
                last_queue_process_time = tv_now;

                size_t checked_loops = 0;
                bool command_sent = false;
                bool any_left_in_queue = false;

                while (checked_loops < global_config.dp_count && !command_sent) {
                    DataPoint *dp = &global_config.datapoints[current_queue_index];

                    if (!dp->is_initialized) {
                        any_left_in_queue = true;

                        if (dp->last_read_sent < 3) {
                            printf("[KNX-INIT] %s (%s) - vprašanje %u/3\n",
                                   dp->opc_path, dp->knx_address, dp->last_read_sent + 1);

                            send_knx_read_request(dp);
                            dp->read_sent_time = now;   // sledljivost: čakamo na odgovor na NAŠE vprašanje
                            dp->read_kind = READ_KIND_INIT;
                            dp->last_read_sent++;
                            dp->last_update_time = now;
                            command_sent = true;
                        } else {
                            printf("[KNX-INIT] Brez odgovora po 3 vprašanjih: %s (%s) - ponovno čez %d min\n",
                                   dp->opc_path, dp->knx_address, RETRY_UNANSWERED_SEC / 60);
                            dp->is_initialized = true;
                            dp->read_unanswered = true;
                            dp->last_update_time = now;
                        }
                    }

                    current_queue_index = (current_queue_index + 1) % global_config.dp_count;
                    checked_loops++;
                }

                if (!any_left_in_queue) {
                    printf("[KNX-INIT] Začetni pregled končan - prehod v normalno obratovanje.\n");
                    queue_completely_finished = true;
                    gettimeofday(&last_smart_poll_tick, NULL);
                }
            }
        }

        // 3. PAMETNO OSVEŽEVANJE: Zažene se šele, ko je začetni scan popolnoma zaključen.
        //    Enako kot začetna vrsta: eno poizvedbo na 200ms takt, NIKOLI usleep() znotraj
        //    zanke po vseh napravah - to bi blokiralo nit in med tem izgubljali dohodne
        //    odgovore (natanko to je povzročalo prejšnji "samo ena/dve napravi na koncu" bug).
        if (knx_connected && queue_completely_finished && global_config.dp_count > 0) {
            struct timeval tv_now2;
            gettimeofday(&tv_now2, NULL);

            long elapsed_ms2 = (tv_now2.tv_sec - last_smart_poll_tick.tv_sec) * 1000 +
                                (tv_now2.tv_usec - last_smart_poll_tick.tv_usec) / 1000;

            if (elapsed_ms2 >= SMART_POLL_INTERVAL_MS) { // redkejši takt, da vodilo ni preobremenjeno
                last_smart_poll_tick = tv_now2;

                size_t checked = 0;
                while (checked < global_config.dp_count) {
                    DataPoint *dp = &global_config.datapoints[smart_poll_cursor];
                    smart_poll_cursor = (smart_poll_cursor + 1) % global_config.dp_count;
                    checked++;

                    long stale_limit = dp->read_unanswered ? RETRY_UNANSWERED_SEC : SMART_POLL_STALE_SEC;
                    if (dp->is_initialized && (now - dp->last_update_time >= stale_limit)) {
                        printf("[KNX-POLL] %s (%s) - brez telegrama že %ld s\n",
                               dp->opc_path, dp->knx_address, (long)(now - dp->last_update_time));
                        send_knx_read_request(dp);
                        dp->read_sent_time = now;   // čakamo na odgovor na NAŠE vprašanje (glej 3c)
                        dp->read_kind = READ_KIND_POLL;
                        dp->last_update_time = now; // preprečimo takojšnje ponovno vrstenje istega elementa
                        break; // ena poizvedba na takt - nadaljujemo od tu na naslednjem taktu
                    }
                }
            }
        }

        // 3b. WATCHDOG: pošiljamo branja, odgovora pa že dolgo ni od nikogar.
        //     Samo opozorilo (ponovna povezava ne popravi zataknjenega rutera).
        if (knx_connected &&
            g_reads_since_response >= WATCHDOG_MIN_READS &&
            (now - g_last_response_time) >= WATCHDOG_SILENCE_SEC &&
            (now - last_watchdog_warning) >= WATCHDOG_REPEAT_SEC) {
            last_watchdog_warning = now;
            char rxbuf[40];
            if (g_last_rx_time) snprintf(rxbuf, sizeof(rxbuf), "pred %ld s", (long)(now - g_last_rx_time));
            else snprintf(rxbuf, sizeof(rxbuf), "se ni bilo");
            printf("[WATCHDOG] %u branj je ostalo brez odgovora, zadnji odgovor pred %ld s; "
                   "zadnji telegram z vodila: %s. "
                   "Telegrami prihajajo, odgovori na branja pa ne: ruter ali vodilo morda ne odgovarja "
                   "(preveri n146, po potrebi ga ponovno zazeni).\n",
                   g_reads_since_response, (long)(now - g_last_response_time), rxbuf);
        }

        // 3c. Sledljivost vprašanj: naše vprašanje, ki v POLL_ANSWER_TIMEOUT_SEC ni dobilo odgovora.
        //     (Odgovori so izpisani kot [KNX-RESP] v handle_knx_traffic.)
        if (knx_connected && now != last_poll_check) {
            last_poll_check = now;
            for (size_t i = 0; i < global_config.dp_count; i++) {
                DataPoint *dp = &global_config.datapoints[i];
                if (dp->read_sent_time && (now - dp->read_sent_time) >= POLL_ANSWER_TIMEOUT_SEC) {
                    if (dp->read_kind == READ_KIND_POLL) {
                        printf("[KNX-POLL] Brez odgovora: %s (%s) - naslednji poskus čez %d min\n",
                               dp->opc_path, dp->knx_address,
                               (int)((dp->read_unanswered ? RETRY_UNANSWERED_SEC : SMART_POLL_STALE_SEC) / 60));
                    }
                    dp->read_sent_time = 0; // odgovor ni prišel: poznejši Response ni več odgovor na to vprašanje
                }
            }
        }

        // 4. Ponovno povezovanje na knxd
        if (!knx_connected && (now - last_reconnect_attempt >= 5)) {
            last_reconnect_attempt = now;
            printf("[KNX] Poskus ponovnega povezovanja...\n");
            knx_fd = init_knx_connection();
            if (knx_fd >= 0) {
                g_knx_fd = knx_fd;
                knx_connected = true;

                // Cas izpada merimo sveze (init_knx_connection() je lahko trajal).
                time_t t_conn = time(NULL);
                long outage = disconnected_since ? (long)(t_conn - disconnected_since) : 0;
                disconnected_since = 0;

                if (need_full_scan || outage >= RESCAN_MIN_OUTAGE_SEC) {
                    // Vrednosti so se med izpadom (knxd, ruter, spanje racunalnika)
                    // lahko spremenile - vse naprave znova preberemo.
                    printf("[KNX] Povezava vzpostavljena po %ld s izpada - popoln ponovni pregled vseh naprav.\n", outage);
                    reset_scan_state(t_conn);
                    queue_completely_finished = false;
                    current_queue_index = 0;
                    smart_poll_cursor = 0;
                    need_full_scan = false;
                    gettimeofday(&last_queue_process_time, NULL);
                    gettimeofday(&last_smart_poll_tick, NULL);
                } else {
                    // Kratek izpad: nadaljujemo kjer smo ostali (vrstica ali smart poll).
                    printf("[KNX] Povezava vzpostavljena po %ld s izpada - ponovni pregled ni potreben.\n", outage);
                    gettimeofday(&last_smart_poll_tick, NULL);
                }
            }
        }
    }

    if (knx_connected) knx_eibd_close();
    return NULL;
}

int main(void) {
    // Pod systemd gre stdout v cevovod: brez tega je izpis blokovno
    // medpomnjen in se v journalu pojavi z zamikom (ali izgine ob sesutju).
    setvbuf(stdout, NULL, _IOLBF, 0);

    // Brez tega bi pisanje v socket, ki ga je knxd ravnokar zaprl,
    // lahko z SIGPIPE tiho ubilo celoten proces.
    signal(SIGPIPE, SIG_IGN);

    // Cist izhod ob Ctrl+C in ob "systemctl stop" (SIGTERM).
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[SYSTEM] Zaganjam KNX <-> OPC UA Prehod...\n");

    if (load_config("config.json") != 0) {
        fprintf(stderr, "[ERROR] Napaka pri nalaganju konfiguracije.\n");
        return 1;
    }

    server = UA_Server_new();
    UA_ServerConfig *opc_config = UA_Server_getConfig(server);

    UA_ServerConfig_setDefault(opc_config);
    init_opcua_server(server, opc_config);

    pthread_t knx_thread;
    if (pthread_create(&knx_thread, NULL, knx_thread_func, NULL) != 0) {
        fprintf(stderr, "[ERROR] Napaka pri ustvarjanju KNX niti!\n");
        return 1;
    }

    printf("[SYSTEM] Prehod uspešno nameščen. Začenjam uradno OPC UA zanko...\n");
    UA_StatusCode retval = UA_Server_run(server, &running);

    running = false;
    pthread_join(knx_thread, NULL);
    UA_Server_delete(server);

    return retval == UA_STATUSCODE_GOOD ? 0 : 1;
}
