/*
  RemoteClient.cpp - исходящее подключение станка к удалённому серверу управления

  См. описание в RemoteClient.h
*/

#include "../Grbl.h"

#if defined(ENABLE_WIFI) && defined(ENABLE_TELNET)

#    include "RemoteClient.h"
#    include "TelnetServer.h"
#    include <WiFi.h>
#    include <WiFiUdp.h>

namespace WebUI {
    static const uint32_t RETRY_INTERVAL_MS  = 15000;  // пауза между попытками подключения
    static const int32_t  CONNECT_TIMEOUT_MS = 1000;  // 1с: меньше столл clientCheckTask при недоступном сервере
    static const uint16_t DISCOVERY_PORT     = 33333;  // UDP-маячок, пока сервер не задан
    static const uint32_t BEACON_INTERVAL_MS = 5000;

    static WiFiClient _remoteClient;
    static WiFiUDP    _beaconUdp;
    static uint32_t   _lastAttempt  = 0;
    static uint32_t   _lastBeacon   = 0;
    static bool       _wasConnected = false;

    // Пока адрес сервера не задан, плата объявляет о себе broadcast-пакетом,
    // чтобы сервер мог найти её в сети и прописать настройки по HTTP
    static void send_discovery_beacon() {
        uint32_t now = millis();
        if ((now - _lastBeacon) < BEACON_INTERVAL_MS) {
            return;
        }
        _lastBeacon = now ? now : 1;

        if (!WiFi.isConnected()) {
            return;
        }

        String beacon = "[DISCOVER:mac=" + WiFi.macAddress() + ",ip=" + WiFi.localIP().toString() +
                        ",name=" + String(remote_machine_name->get()) + ",fw=grbl" + GRBL_VERSION + "-" + GRBL_VERSION_BUILD + "]";
        _beaconUdp.beginPacket(IPAddress(255, 255, 255, 255), DISCOVERY_PORT);
        _beaconUdp.write((const uint8_t*)beacon.c_str(), beacon.length());
        _beaconUdp.endPacket();
    }

    void Remote_Client::begin() {
        _lastAttempt  = 0;
        _wasConnected = false;
    }

    void Remote_Client::end() {
        if (_remoteClient.connected()) {
            _remoteClient.stop();
        }
        _wasConnected = false;
    }

    void Remote_Client::handle() {
        // clientCheckTask может дёрнуть handle() раньше, чем make_settings()
        // создаст объекты настроек
        if (remote_server_address == NULL) {
            return;
        }

        const char* server = remote_server_address->get();
        if (*server == '\0') {  // адрес очищен — назад в режим обнаружения (маяк)
            if (_wasConnected) {  // оборвать живой исходящий канал перед маяком
                _wasConnected = false;
                _remoteClient.stop();
                grbl_send(CLIENT_ALL, "[MSG:REMOTE Disconnected]\r\n");
            }
            send_discovery_beacon();
            return;
        }

        if (WiFi.isConnected() && _remoteClient.connected()) {  // канал жив только при живом STA;
            return;                                             // иначе падаем в teardown ниже
        }

        if (_wasConnected) {
            _wasConnected = false;
            _remoteClient.stop();
            grbl_send(CLIENT_ALL, "[MSG:REMOTE Disconnected]\r\n");
        }

        uint32_t now = millis();
        if (_lastAttempt != 0 && (now - _lastAttempt) < RETRY_INTERVAL_MS) {
            return;
        }
        _lastAttempt = now ? now : 1;

        if (!WiFi.isConnected()) {  // нужно активное STA-подключение к роутеру
            return;
        }

        if (!Telnet_Server::has_free_slot()) {  // нет свободного telnet-слота — не штурмуем сервер
            return;                             // (анти-storm: иначе connect+HELLO+reject каждые 15с)
        }

        String   addr(server);
        int      colon = addr.lastIndexOf(':');
        uint16_t port  = (colon > 0) ? addr.substring(colon + 1).toInt() : 0;
        if (port == 0) {
            grbl_send(CLIENT_ALL, "[MSG:REMOTE Bad server address, expected host:port]\r\n");
            return;
        }
        String host = addr.substring(0, colon);

        IPAddress serverIp;
        bool      connected;
        if (serverIp.fromString(host)) {
            connected = _remoteClient.connect(serverIp, port, CONNECT_TIMEOUT_MS);  // IP -> без DNS-столла
        } else {
            connected = _remoteClient.connect(host.c_str(), port, CONNECT_TIMEOUT_MS);
        }
        if (!connected) {
            return;  // тихий повтор через RETRY_INTERVAL_MS
        }

        // авторизационная строка: по токену сервер определяет, что это за станок,
        // name — человекочитаемое имя для оператора
        String hello = "[HELLO:token=" + String(remote_machine_token->get()) + ",name=" + String(remote_machine_name->get()) +
                       ",mac=" + WiFi.macAddress() + "]\r\n";
        _remoteClient.write((const uint8_t*)hello.c_str(), hello.length());

        if (!Telnet_Server::attach_client(_remoteClient)) {
            _remoteClient.stop();  // все слоты заняты входящими подключениями
            return;
        }

        _wasConnected = true;
        grbl_sendf(CLIENT_ALL, "[MSG:REMOTE Connected to %s]\r\n", server);
    }
}
#endif  // ENABLE_WIFI && ENABLE_TELNET
