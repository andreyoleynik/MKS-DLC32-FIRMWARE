/*
  WebServer.cpp -  web server functions class

  Copyright (c) 2014 Luc Lebosse. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "../Grbl.h"

#if defined(ENABLE_WIFI) && defined(ENABLE_HTTP)

#    include "WifiServices.h"

#    include "ESPResponse.h"
#    include "Serial2Socket.h"
#    include "WebServer.h"
#    include <WebSocketsServer.h>
#    include <WiFi.h>
#    include <FS.h>
#    include <SPIFFS.h>
#    ifdef ENABLE_SD_CARD
#        include <SD.h>
#        include "../SDCard.h"
#    endif
#    include <WebServer.h>
#    include <ESP32SSDP.h>
#    include <StreamString.h>
#    include <strings.h>
#    include <Update.h>
#    include <esp_wifi_types.h>
#    include <esp_partition.h>
#    include <lwip/sockets.h>
#    include <string.h>
#    if !defined(CONFIG_ESP32_ENABLE_COREDUMP_TO_NONE)
#        include <esp_core_dump.h>
#    endif
#    ifdef ENABLE_MDNS
#        include <ESPmDNS.h>
#    endif
#    ifdef ENABLE_SSDP
#        include <ESP32SSDP.h>
#    endif
#    ifdef ENABLE_CAPTIVE_PORTAL
#        include <DNSServer.h>

namespace WebUI {
#    if defined(CONFIG_ESP32_ENABLE_COREDUMP_TO_NONE)
    static constexpr bool CORE_DUMP_API_AVAILABLE = false;
#    else
    static constexpr bool CORE_DUMP_API_AVAILABLE = true;
#    endif

    const byte DNS_PORT = 53;
    DNSServer  dnsServer;
}

#    endif
#    include <esp_ota_ops.h>

//embedded response file if no files on SPIFFS
#    include "NoFile.h"

namespace WebUI {
    //Default 404
    const char PAGE_404[] =
        "<HTML>\n<HEAD>\n<title>Redirecting...</title> \n</HEAD>\n<BODY>\n<CENTER>Unknown page : $QUERY$- you will be "
        "redirected...\n<BR><BR>\nif not redirected, <a href='http://$WEB_ADDRESS$'>click here</a>\n<BR><BR>\n<PROGRESS name='prg' "
        "id='prg'></PROGRESS>\n\n<script>\nvar i = 0; \nvar x = document.getElementById(\"prg\"); \nx.max=10; \nvar "
        "interval=setInterval(function(){\ni=i+1; \nvar x = document.getElementById(\"prg\"); \nx.value=i; \nif (i>10) "
        "\n{\nclearInterval(interval);\nwindow.location.href='/';\n}\n},1000);\n</script>\n</CENTER>\n</BODY>\n</HTML>\n\n";
    const char PAGE_CAPTIVE[] =
        "<HTML>\n<HEAD>\n<title>Captive Portal</title> \n</HEAD>\n<BODY>\n<CENTER>Captive Portal page : $QUERY$- you will be "
        "redirected...\n<BR><BR>\nif not redirected, <a href='http://$WEB_ADDRESS$'>click here</a>\n<BR><BR>\n<PROGRESS name='prg' "
        "id='prg'></PROGRESS>\n\n<script>\nvar i = 0; \nvar x = document.getElementById(\"prg\"); \nx.max=10; \nvar "
        "interval=setInterval(function(){\ni=i+1; \nvar x = document.getElementById(\"prg\"); \nx.value=i; \nif (i>10) "
        "\n{\nclearInterval(interval);\nwindow.location.href='/';\n}\n},1000);\n</script>\n</CENTER>\n</BODY>\n</HTML>\n\n";

    // Error codes for upload
    const int ESP_ERROR_AUTHENTICATION   = 1;
    const int ESP_ERROR_FILE_CREATION    = 2;
    const int ESP_ERROR_FILE_WRITE       = 3;
    const int ESP_ERROR_UPLOAD           = 4;
    const int ESP_ERROR_NOT_ENOUGH_SPACE = 5;
    const int ESP_ERROR_UPLOAD_CANCELLED = 6;
    const int ESP_ERROR_FILE_CLOSE       = 7;

    static const char* sd_state_text(SDState state) {
        switch (state) {
            case SDState::Idle: return "Idle";
            case SDState::NotPresent: return "No SD card";
            case SDState::Busy: return "Busy printing";
            case SDState::BusyUploading: return "Busy uploading";
            case SDState::BusyParsing: return "Busy parsing";
            default: return "Unknown SD state";
        }
    }

    static const char* upload_status_text(uint8_t status) {
        switch (status) {
            case UPLOAD_FILE_START: return "START";
            case UPLOAD_FILE_WRITE: return "WRITE";
            case UPLOAD_FILE_END: return "END";
            case UPLOAD_FILE_ABORTED: return "ABORTED";
            default: return "UNKNOWN";
        }
    }

    static const char* sys_state_text(State state) {
        switch (state) {
            case State::Idle: return "Idle";
            case State::Alarm: return "Alarm";
            case State::CheckMode: return "CheckMode";
            case State::Homing: return "Homing";
            case State::Cycle: return "Cycle";
            case State::Hold: return "Hold";
            case State::Jog: return "Jog";
            case State::SafetyDoor: return "SafetyDoor";
            case State::Sleep: return "Sleep";
            default: return "Unknown";
        }
    }

    static void format_bytes_to_text(uint64_t bytes, char* out, size_t out_len) {
        if (out_len == 0) {
            return;
        }

        if (bytes < 1024ULL) {
            snprintf(out, out_len, "%u B", (unsigned)bytes);
        } else if (bytes < (1024ULL * 1024ULL)) {
            snprintf(out, out_len, "%.2f KB", (double)bytes / 1024.0);
        } else if (bytes < (1024ULL * 1024ULL * 1024ULL)) {
            snprintf(out, out_len, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
        } else {
            snprintf(out, out_len, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
        }
    }

    Web_Server        web_server;
    bool              Web_Server::_setupdone     = false;
    uint16_t          Web_Server::_port          = 0;
    long              Web_Server::_id_connection = 0;
    UploadStatusType  Web_Server::_upload_status = UploadStatusType::NONE;
    WebServer*        Web_Server::_webserver     = NULL;
    WebSocketsServer* Web_Server::_socket_server = NULL;
#    ifdef ENABLE_AUTHENTICATION
    AuthenticationIP* Web_Server::_head  = NULL;
    uint8_t           Web_Server::_nb_ip = 0;
    const int         MAX_AUTH_IP        = 10;
#    endif
    Web_Server::Web_Server() {}
    Web_Server::~Web_Server() { end(); }

    long Web_Server::get_client_ID() { return _id_connection; }

    bool Web_Server::begin() {
        bool no_error = true;
        _setupdone    = false;
        if (http_enable->get() == 0) {
            return false;
        }
        _port = http_port->get();

        //create instance
        _webserver = new WebServer(_port);
#    ifdef ENABLE_AUTHENTICATION
        //here the list of headers to be recorded
        const char* headerkeys[]   = { "Cookie" };
        size_t      headerkeyssize = sizeof(headerkeys) / sizeof(char*);
        //ask server to track these headers
        _webserver->collectHeaders(headerkeys, headerkeyssize);
#    endif
        _socket_server = new WebSocketsServer(_port + 1);
        _socket_server->begin();
        _socket_server->onEvent(handle_Websocket_Event);

        //Websocket output
        Serial2Socket.attachWS(_socket_server);

        //events functions
        //_web_events->onConnect(handle_onevent_connect);
        //events management
        // _webserver->addHandler(_web_events);

        //Websocket function
        //_web_socket->onEvent(handle_Websocket_Event);
        //Websocket management
        //_webserver->addHandler(_web_socket);

        //Web server handlers
        //trick to catch command line on "/" before file being processed
        _webserver->on("/", HTTP_ANY, handle_root);

        //Page not found handler
        _webserver->onNotFound(handle_not_found);

        //need to be there even no authentication to say to UI no authentication
        _webserver->on("/login", HTTP_ANY, handle_login);

        //web commands
        _webserver->on("/command", HTTP_ANY, handle_web_command);
        _webserver->on("/command_silent", HTTP_ANY, handle_web_command_silent);
        _webserver->on("/coredump/info", HTTP_GET, handle_coredump_info);
        _webserver->on("/coredump.bin", HTTP_GET, handle_coredump_download);

        //SPIFFS
        _webserver->on("/files", HTTP_ANY, handleFileList, SPIFFSFileupload);

        //web update
        _webserver->on("/updatefw", HTTP_ANY, handleUpdate, WebUpdateUpload);

#    ifdef ENABLE_SD_CARD
        //Direct SD management
        _webserver->on("/upload", HTTP_ANY, handle_direct_SDFileList, SDFile_direct_upload);
        // _webserver->on("/SD", HTTP_ANY, handle_SDCARD);
#    endif

#    ifdef ENABLE_CAPTIVE_PORTAL
        if (WiFi.getMode() == WIFI_AP) {
            // if DNSServer is started with "*" for domain name, it will reply with
            // provided IP to all DNS request
            dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
            grbl_send(CLIENT_ALL, "[MSG:Captive Portal Started]\r\n");
            _webserver->on("/generate_204", HTTP_ANY, handle_root);
            _webserver->on("/gconnectivitycheck.gstatic.com", HTTP_ANY, handle_root);
            //do not forget the / at the end
            _webserver->on("/fwlink/", HTTP_ANY, handle_root);
        }
#    endif

#    ifdef ENABLE_SSDP
        //SSDP service presentation
        if (WiFi.getMode() == WIFI_STA) {
            _webserver->on("/description.xml", HTTP_GET, handle_SSDP);
            //Add specific for SSDP
            SSDP.setSchemaURL("description.xml");
            SSDP.setHTTPPort(_port);
            SSDP.setName(wifi_config.Hostname());
            SSDP.setURL("/");
            SSDP.setDeviceType("upnp:rootdevice");
            /*Any customization could be here
        SSDP.setModelName (ESP32_MODEL_NAME);
        SSDP.setModelURL (ESP32_MODEL_URL);
        SSDP.setModelNumber (ESP_MODEL_NUMBER);
        SSDP.setManufacturer (ESP_MANUFACTURER_NAME);
        SSDP.setManufacturerURL (ESP_MANUFACTURER_URL);
        */

            //Start SSDP
            grbl_send(CLIENT_ALL, "[MSG:SSDP Started]\r\n");
            SSDP.begin();
        }
#    endif
        grbl_send(CLIENT_ALL, "[MSG:HTTP Started]\r\n");
        //start webserver
        _webserver->begin();
#    ifdef ENABLE_MDNS
        //add mDNS (и в STA, и в AP — согласовано с MDNS.begin в WifiServices)
        if (WiFi.getMode() == WIFI_STA || WiFi.getMode() == WIFI_AP) {
            MDNS.addService("http", "tcp", _port);
        }
#    endif
        _setupdone = true;
        return no_error;
    }

    void Web_Server::end() {
        _setupdone = false;
#    ifdef ENABLE_SSDP
        SSDP.end();
#    endif  //ENABLE_SSDP
#    ifdef ENABLE_MDNS
        //remove mDNS
        mdns_service_remove("_http", "_tcp");
#    endif
        if (_socket_server) {
            // Сначала отцепить Serial2Socket: после end() ещё идут grbl_send(CLIENT_ALL)
            // (StopWiFi/StartSTA), и flush() по висячему указателю = use-after-free.
            Serial2Socket.detachWS();
            delete _socket_server;
            _socket_server = NULL;
        }

        if (_webserver) {
            delete _webserver;
            _webserver = NULL;
        }

#    ifdef ENABLE_AUTHENTICATION
        while (_head) {
            AuthenticationIP* current = _head;
            _head                     = _head->_next;
            delete current;
        }
        _nb_ip = 0;
#    endif
    }

    //Root of Webserver/////////////////////////////////////////////////////

    void Web_Server::handle_root() {
        String path        = "/index.html";
        String contentType = getContentType(path);
        String pathWithGz  = path + ".gz";
        //if have a index.html or gzip version this is default root page
        if ((SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) && !_webserver->hasArg("index") &&  // forcefallback
            _webserver->arg("index") != "yes") {
            if (SPIFFS.exists(pathWithGz)) {
                path = pathWithGz;
            }
            WiFiClient client = _webserver->client();
            client.setNoDelay(true);
            File file = SPIFFS.open(path, FILE_READ);
            if (!file) {
                _webserver->send(500, "text/plain", "WebUI open failed");
                return;
            }
            size_t totalFileSize = file.size();
            size_t sentBytes     = 0;

            _webserver->setContentLength(totalFileSize);
            if (path.endsWith(".gz")) {
                _webserver->sendHeader("Content-Encoding", "gzip");
            }
            _webserver->send(200, contentType, "");

            uint8_t buf[1024];
            while (sentBytes < totalFileSize) {
                int chunk = file.read(buf, sizeof(buf));
                if (chunk <= 0) {
                    break;
                }
                _webserver->client().write(buf, chunk);
                sentBytes += (size_t)chunk;
                COMMANDS::wait(0);
            }
            file.close();
            return;
        }

        //if no lets launch the default content
        grbl_send(CLIENT_ALL, "[MSG:WebUI files missing in SPIFFS]\r\n");
        _webserver->sendHeader("Content-Encoding", "gzip");
        _webserver->send_P(200, "text/html", PAGE_NOFILES, PAGE_NOFILES_SIZE);
    }

    //Handle not registred path on SPIFFS neither SD ///////////////////////
    void Web_Server::handle_not_found() {
        if (is_authenticated() == AuthenticationLevel::LEVEL_GUEST) {
            _webserver->sendContent_P("HTTP/1.1 301 OK\r\nLocation: /\r\nCache-Control: no-cache\r\n\r\n");
            //_webserver->client().stop();
            return;
        }

        bool   page_not_found = false;
        String path           = _webserver->urlDecode(_webserver->uri());
        String contentType    = getContentType(path);
        String pathWithGz     = path + ".gz";

#    ifdef ENABLE_SD_CARD
        if ((path.substring(0, 4) == "/SD/")) {
            //remove /SD
            path = path.substring(3);
            if (SDState::Idle != get_sd_state(true)) {
                String content = "cannot open: ";
                content += path + ", SD is not available.";

                _webserver->send(500, "text/plain", content);
            }
            if (SD.exists(pathWithGz) || SD.exists(path)) {
                set_sd_state(SDState::BusyUploading);
                if (SD.exists(pathWithGz)) {
                    path = pathWithGz;
                }
                File datafile = SD.open(path);
                if (datafile) {
                    vTaskDelay(1 / portTICK_RATE_MS);
                    size_t totalFileSize = datafile.size();
                    size_t i             = 0;
                    bool   done          = false;
                    _webserver->setContentLength(totalFileSize);
                    _webserver->send(200, contentType, "");
                    uint8_t buf[1024];
                    while (!done) {
                        vTaskDelay(1 / portTICK_RATE_MS);
                        int v = datafile.read(buf, 1024);
                        if ((v == -1) || (v == 0)) {
                            done = true;
                        } else {
                            _webserver->client().write(buf, v);
                            i += v;
                        }

                        if (i >= totalFileSize) {
                            done = true;
                        }
                    }
                    datafile.close();
                    if (i != totalFileSize) {
                        //error: TBD
                    }
                    set_sd_state(SDState::Idle);
                    return;
                }
                set_sd_state(SDState::Idle);
            }
            String content = "cannot find ";
            content += path;
            _webserver->send(404, "text/plain", content);
            return;
        } else
#    endif
            if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
            if (SPIFFS.exists(pathWithGz)) {
                path = pathWithGz;
            }
            WiFiClient client = _webserver->client();
            client.setNoDelay(true);
            File file = SPIFFS.open(path, FILE_READ);
            if (!file) {
                _webserver->send(404, "text/plain", "Not found");
                return;
            }
            _webserver->streamFile(file, contentType);
            file.close();
            return;
        } else {
            page_not_found = true;
        }

        if (page_not_found) {
#    ifdef ENABLE_CAPTIVE_PORTAL
            if (WiFi.getMode() == WIFI_AP) {
                String contentType = PAGE_CAPTIVE;
                String stmp        = WiFi.softAPIP().toString();
                //Web address = ip + port
                String KEY_IP    = "$WEB_ADDRESS$";
                String KEY_QUERY = "$QUERY$";
                if (_port != 80) {
                    stmp += ":";
                    stmp += String(_port);
                }
                contentType.replace(KEY_IP, stmp);
                contentType.replace(KEY_IP, stmp);
                contentType.replace(KEY_QUERY, _webserver->uri());
                _webserver->send(200, "text/html", contentType);
                //_webserver->sendContent_P(NOT_AUTH_NF);
                //_webserver->client().stop();
                return;
            }
#    endif
            path        = "/404.htm";
            contentType = getContentType(path);
            pathWithGz  = path + ".gz";
            if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
                if (SPIFFS.exists(pathWithGz)) {
                    path = pathWithGz;
                }
                WiFiClient client = _webserver->client();
                client.setNoDelay(true);
                File file = SPIFFS.open(path, FILE_READ);
                if (!file) {
                    _webserver->send(404, "text/plain", "Not found");
                    return;
                }
                _webserver->streamFile(file, contentType);
                file.close();

            } else {
                //if not template use default page
                contentType = PAGE_404;
                String stmp;
                if (WiFi.getMode() == WIFI_STA) {
                    stmp = WiFi.localIP().toString();
                } else {
                    stmp = WiFi.softAPIP().toString();
                }
                //Web address = ip + port
                String KEY_IP    = "$WEB_ADDRESS$";
                String KEY_QUERY = "$QUERY$";
                if (_port != 80) {
                    stmp += ":";
                    stmp += String(_port);
                }
                contentType.replace(KEY_IP, stmp);
                contentType.replace(KEY_QUERY, _webserver->uri());
                _webserver->send(200, "text/html", contentType);
            }
        }
    }

#    ifdef ENABLE_SSDP
    //http SSDP xml presentation
    void Web_Server::handle_SSDP() {
        StreamString sschema;
        if (sschema.reserve(1024)) {
            String templ = "<?xml version=\"1.0\"?>"
                           "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
                           "<specVersion>"
                           "<major>1</major>"
                           "<minor>0</minor>"
                           "</specVersion>"
                           "<URLBase>http://%s:%u/</URLBase>"
                           "<device>"
                           "<deviceType>upnp:rootdevice</deviceType>"
                           "<friendlyName>%s</friendlyName>"
                           "<presentationURL>/</presentationURL>"
                           "<serialNumber>%s</serialNumber>"
                           "<modelName>ESP32</modelName>"
                           "<modelNumber>Marlin</modelNumber>"
                           "<modelURL>http://espressif.com/en/products/hardware/esp-wroom-32/overview</modelURL>"
                           "<manufacturer>Espressif Systems</manufacturer>"
                           "<manufacturerURL>http://espressif.com</manufacturerURL>"
                           "<UDN>uuid:%s</UDN>"
                           "</device>"
                           "</root>\r\n"
                           "\r\n";
            char     uuid[37];
            String   sip    = WiFi.localIP().toString();
            uint32_t chipId = (uint16_t)(ESP.getEfuseMac() >> 32);
            sprintf(uuid,
                    "38323636-4558-4dda-9188-cda0e6%02x%02x%02x",
                    (uint16_t)((chipId >> 16) & 0xff),
                    (uint16_t)((chipId >> 8) & 0xff),
                    (uint16_t)chipId & 0xff);
            String serialNumber = String(chipId);
            sschema.printf(templ.c_str(), sip.c_str(), _port, wifi_config.Hostname().c_str(), serialNumber.c_str(), uuid);
            _webserver->send(200, "text/xml", (String)sschema);
        } else {
            _webserver->send(500);
        }
    }
#    endif

    void Web_Server::_handle_web_command(bool silent) {
        //to save time if already disconnected
        //if (_webserver->hasArg ("PAGEID") ) {
        //    if (_webserver->arg ("PAGEID").length() > 0 ) {
        //       if (_webserver->arg ("PAGEID").toInt() != _id_connection) {
        //       _webserver->send (200, "text/plain", "Invalid command");
        //       return;
        //       }
        //    }
        //}
        AuthenticationLevel auth_level = is_authenticated();
        String              cmd;
        if (_webserver->hasArg("plain")) {
            cmd = _webserver->arg("plain");
        } else if (_webserver->hasArg("commandText")) {
            cmd = _webserver->arg("commandText");
        } else {
            _webserver->send(200, "text/plain", "Invalid command");
            return;
        }
        // Keep raw bytes (e.g. 0x18) intact for realtime commands.
        // Use a trimmed copy only for internal [ESPxxx] parsing.
        String espCmd = cmd;
        espCmd.trim();
        int ESPpos = espCmd.indexOf("[ESP");
        if (ESPpos > -1) {
            char line[256];
            strncpy(line, espCmd.c_str(), 255);
            ESPResponseStream* espresponse = silent ? NULL : new ESPResponseStream(_webserver);
            Error              err         = system_execute_line(line, espresponse, auth_level);
            char               answer[96];
            if (err == Error::Ok) {
                snprintf(answer, sizeof(answer), "ok");
            } else {
                const char* msg = errorString(err);
                if (msg) {
                    snprintf(answer, sizeof(answer), "Error: %s", msg);
                } else {
                    snprintf(answer, sizeof(answer), "Error: %d", static_cast<int>(err));
                }
            }
            if (silent || !espresponse->anyOutput()) {
                _webserver->send(err != Error::Ok ? 500 : 200, "text/plain", answer);
            } else {
                espresponse->flush();
            }
            if(espresponse) delete(espresponse);
        } else {  //execute GCODE
            if (auth_level == AuthenticationLevel::LEVEL_GUEST) {
                _webserver->send(401, "text/plain", "Authentication failed!\n");
                return;
            }
            // Split once in linear time to avoid O(n^2) temporary String churn.
            bool hasError = false;
            int  lineStart = 0;
            int  cmdLen    = cmd.length();
            const char* cmdData = cmd.c_str();
            String scmd;
            scmd.reserve(258);
            for (int i = 0; i <= cmdLen; i++) {
                if (i != cmdLen && cmd[i] != '\n') {
                    continue;
                }

                int lineLen = i - lineStart;
                if (lineLen <= 0) {
                    lineStart = i + 1;
                    continue;
                }

                scmd.remove(0);
                scmd.concat(cmdData + lineStart, (unsigned int)lineLen);
                // 0xC2 is an HTML encoding prefix that, in UTF-8 mode,
                // precede 0x90 and 0xa0-0bf, which are GRBL realtime commands.
                // There are other encodings for 0x91-0x9f, so I am not sure
                // how - or whether - those commands work.
                // Ref: https://www.w3schools.com/tags/ref_urlencode.ASP
                if (!silent && (scmd.length() == 2) && ((uint8_t)scmd[0] == 0xC2)) {
                    scmd[0] = scmd[1];
                    scmd.remove(1, 1);
                }
                if (scmd.length() > 1) {
                    scmd += "\n";
                } else if (!is_realtime_command((uint8_t)scmd[0])) {
                    scmd += "\n";
                }
                if (!Serial2Socket.push(scmd.c_str())) {
                    hasError = true;
                }

                lineStart = i + 1;
            }
            _webserver->send(200, "text/plain", hasError?"Error":"");
        }
    }

    void Web_Server::handle_coredump_info() {
        if (is_authenticated() == AuthenticationLevel::LEVEL_GUEST) {
            _webserver->send(401, "application/json", "{\"status\":\"error\",\"message\":\"Authentication failed\"}");
            return;
        }

        size_t    image_addr = 0;
        size_t    image_size = 0;
        esp_err_t err        = ESP_ERR_NOT_SUPPORTED;
        if (CORE_DUMP_API_AVAILABLE) {
    #if !defined(CONFIG_ESP32_ENABLE_COREDUMP_TO_NONE)
            err = esp_core_dump_image_get(&image_addr, &image_size);
    #endif
        }
        bool      has_image  = (err == ESP_OK) && (image_size > 0);

        const esp_partition_t* core_part =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        const char* configured_to_flash = "false";
#if defined(CONFIG_ESP32_ENABLE_COREDUMP_TO_FLASH)
        configured_to_flash             = "true";
#endif

        char json[384];
        if (core_part) {
            snprintf(json,
                 sizeof(json),
                 "{\"status\":\"ok\",\"coreDumpApiAvailable\":%s,\"configuredToFlash\":%s,\"partitionPresent\":true,\"imageAvailable\":%s,\"espErr\":%d,\"address\":%u,\"size\":%u,\"partitionAddress\":%u,\"partitionSize\":%u}",
                 CORE_DUMP_API_AVAILABLE ? "true" : "false",
                 configured_to_flash,
                 has_image ? "true" : "false",
                 (int)err,
                 (unsigned)image_addr,
                 (unsigned)image_size,
                 (unsigned)core_part->address,
                 (unsigned)core_part->size);
        } else {
            snprintf(json,
                 sizeof(json),
                 "{\"status\":\"ok\",\"coreDumpApiAvailable\":%s,\"configuredToFlash\":%s,\"partitionPresent\":false,\"imageAvailable\":%s,\"espErr\":%d,\"address\":%u,\"size\":%u}",
                 CORE_DUMP_API_AVAILABLE ? "true" : "false",
                 configured_to_flash,
                 has_image ? "true" : "false",
                 (int)err,
                 (unsigned)image_addr,
                 (unsigned)image_size);
        }

        _webserver->send(200, "application/json", json);
    }

    void Web_Server::handle_coredump_download() {
        if (is_authenticated() == AuthenticationLevel::LEVEL_GUEST) {
            _webserver->send(401, "text/plain", "Authentication failed");
            return;
        }

        if (!CORE_DUMP_API_AVAILABLE) {
            _webserver->send(501, "text/plain", "Core dump is disabled in this firmware build");
            return;
        }

        size_t    image_addr = 0;
        size_t    image_size = 0;
    #if !defined(CONFIG_ESP32_ENABLE_COREDUMP_TO_NONE)
        esp_err_t err        = esp_core_dump_image_get(&image_addr, &image_size);
    #else
        esp_err_t err        = ESP_ERR_NOT_SUPPORTED;
    #endif
        if ((err != ESP_OK) || (image_size == 0)) {
            _webserver->send(404, "text/plain", "No core dump image in flash");
            return;
        }

        const esp_partition_t* core_part =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        if (!core_part) {
            _webserver->send(500, "text/plain", "coredump partition not found");
            return;
        }

        if ((image_addr < core_part->address) || ((image_addr + image_size) > (core_part->address + core_part->size))) {
            _webserver->send(500, "text/plain", "core dump image outside coredump partition");
            return;
        }

        size_t offset = 0;
        if (_webserver->hasArg("offset")) {
            offset = strtoul(_webserver->arg("offset").c_str(), NULL, 10);
        }
        if (offset >= image_size) {
            _webserver->send(416, "text/plain", "offset out of range");
            return;
        }

        size_t length = image_size - offset;
        if (_webserver->hasArg("length")) {
            size_t requested = strtoul(_webserver->arg("length").c_str(), NULL, 10);
            if ((requested > 0) && (requested < length)) {
                length = requested;
            }
        }

        size_t part_offset = (image_addr - core_part->address) + offset;

        _webserver->sendHeader("Content-Disposition", "attachment; filename=\"coredump.bin\"");
        _webserver->setContentLength(length);
        _webserver->send(200, "application/octet-stream", "");

        uint8_t buf[1024];
        size_t  sent = 0;
        while (sent < length) {
            size_t chunk = (length - sent) > sizeof(buf) ? sizeof(buf) : (length - sent);
            if (esp_partition_read(core_part, part_offset + sent, buf, chunk) != ESP_OK) {
                grbl_send(CLIENT_SERIAL, "[MSG:Core dump read failed]\r\n");
                break;
            }
            _webserver->client().write(buf, chunk);
            sent += chunk;
            vTaskDelay(1 / portTICK_RATE_MS);
        }
    }

    //login status check
    void Web_Server::handle_login() {
#    ifdef ENABLE_AUTHENTICATION
        String smsg;
        String sUser, sPassword;
        String auths;
        int    code            = 200;
        bool   msg_alert_error = false;
        //disconnect can be done anytime no need to check credential
        if (_webserver->hasArg("DISCONNECT")) {
            String cookie = _webserver->header("Cookie");
            int    pos    = cookie.indexOf("ESPSESSIONID=");
            String sessionID;
            if (pos != -1) {
                int pos2  = cookie.indexOf(";", pos);
                sessionID = cookie.substring(pos + strlen("ESPSESSIONID="), pos2);
            }
            ClearAuthIP(_webserver->client().remoteIP(), sessionID.c_str());
            _webserver->sendHeader("Set-Cookie", "ESPSESSIONID=0");
            _webserver->sendHeader("Cache-Control", "no-cache");
            _webserver->send(code, "application/json", "{\"status\":\"Ok\",\"authentication_lvl\":\"guest\"}");
            //_webserver->client().stop();
            return;
        }

        AuthenticationLevel auth_level = is_authenticated();
        if (auth_level == AuthenticationLevel::LEVEL_GUEST) {
            auths = "guest";
        } else if (auth_level == AuthenticationLevel::LEVEL_USER) {
            auths = "user";
        } else if (auth_level == AuthenticationLevel::LEVEL_ADMIN) {
            auths = "admin";
        } else {
            auths = "???";
        }

        //check is it is a submission or a query
        if (_webserver->hasArg("SUBMIT")) {
            //is there a correct list of query?
            if (_webserver->hasArg("PASSWORD") && _webserver->hasArg("USER")) {
                //USER
                sUser = _webserver->arg("USER");
                if (!((sUser == DEFAULT_ADMIN_LOGIN) || (sUser == DEFAULT_USER_LOGIN))) {
                    msg_alert_error = true;
                    smsg            = "Error : Incorrect User";
                    code            = 401;
                }

                if (msg_alert_error == false) {
                    //Password
                    sPassword             = _webserver->arg("PASSWORD");
                    String sadminPassword = admin_password->get();
                    String suserPassword  = user_password->get();

                    if (!(sUser == DEFAULT_ADMIN_LOGIN && sPassword == sadminPassword) ||
                        (sUser == DEFAULT_USER_LOGIN && sPassword == suserPassword)) {
                        msg_alert_error = true;
                        smsg            = "Error: Incorrect password";
                        code            = 401;
                    }
                }
            } else {
                msg_alert_error = true;
                smsg            = "Error: Missing data";
                code            = 500;
            }
            //change password
            if (_webserver->hasArg("PASSWORD") && _webserver->hasArg("USER") && _webserver->hasArg("NEWPASSWORD") &&
                (msg_alert_error == false)) {
                String newpassword = _webserver->arg("NEWPASSWORD");

                char pwdbuf[MAX_LOCAL_PASSWORD_LENGTH + 1];
                newpassword.toCharArray(pwdbuf, MAX_LOCAL_PASSWORD_LENGTH + 1);

                if (COMMANDS::isLocalPasswordValid(pwdbuf)) {
                    Error err;

                    if (sUser == DEFAULT_ADMIN_LOGIN) {
                        err = admin_password->setStringValue(pwdbuf);
                    } else {
                        err = user_password->setStringValue(pwdbuf);
                    }
                    if (err != Error::Ok) {
                        msg_alert_error = true;
                        smsg            = "Error: Cannot apply changes";
                        code            = 500;
                    }
                } else {
                    msg_alert_error = true;
                    smsg            = "Error: Incorrect password";
                    code            = 500;
                }
            }
            if ((code == 200) || (code == 500)) {
                AuthenticationLevel current_auth_level;
                if (sUser == DEFAULT_ADMIN_LOGIN) {
                    current_auth_level = AuthenticationLevel::LEVEL_ADMIN;
                } else if (sUser == DEFAULT_USER_LOGIN) {
                    current_auth_level = AuthenticationLevel::LEVEL_USER;
                } else {
                    current_auth_level = AuthenticationLevel::LEVEL_GUEST;
                }
                //create Session
                if ((current_auth_level != auth_level) || (auth_level == AuthenticationLevel::LEVEL_GUEST)) {
                    AuthenticationIP* current_auth = new AuthenticationIP;
                    current_auth->level            = current_auth_level;
                    current_auth->ip               = _webserver->client().remoteIP();
                    strcpy(current_auth->sessionID, create_session_ID());
                    strcpy(current_auth->userID, sUser.c_str());
                    current_auth->last_time = millis();
                    if (AddAuthIP(current_auth)) {
                        String tmps = "ESPSESSIONID=";
                        tmps += current_auth->sessionID;
                        _webserver->sendHeader("Set-Cookie", tmps);
                        _webserver->sendHeader("Cache-Control", "no-cache");
                        switch (current_auth->level) {
                            case AuthenticationLevel::LEVEL_ADMIN:
                                auths = "admin";
                                break;
                            case AuthenticationLevel::LEVEL_USER:
                                auths = "user";
                                break;
                            default:
                                auths = "guest";
                                break;
                        }
                    } else {
                        delete current_auth;
                        msg_alert_error = true;
                        code            = 500;
                        smsg            = "Error: Too many connections";
                    }
                }
            }
            if (code == 200) {
                smsg = "Ok";
            }

            char json[160];
            snprintf(json,
                     sizeof(json),
                     "{\"status\":\"%s\",\"authentication_lvl\":\"%s\"}",
                     smsg.c_str(),
                     auths.c_str());
            _webserver->send(code, "application/json", json);
        } else {
            if (auth_level != AuthenticationLevel::LEVEL_GUEST) {
                String cookie = _webserver->header("Cookie");
                int    pos    = cookie.indexOf("ESPSESSIONID=");
                String sessionID;
                if (pos != -1) {
                    int pos2                            = cookie.indexOf(";", pos);
                    sessionID                           = cookie.substring(pos + strlen("ESPSESSIONID="), pos2);
                    AuthenticationIP* current_auth_info = GetAuth(_webserver->client().remoteIP(), sessionID.c_str());
                    if (current_auth_info != NULL) {
                        sUser = current_auth_info->userID;
                    }
                }
            }
            char json[192];
            snprintf(json,
                     sizeof(json),
                     "{\"status\":\"200\",\"authentication_lvl\":\"%s\",\"user\":\"%s\"}",
                     auths.c_str(),
                     sUser.c_str());
            _webserver->send(code, "application/json", json);
        }
#    else
        _webserver->sendHeader("Cache-Control", "no-cache");
        _webserver->send(200, "application/json", "{\"status\":\"Ok\",\"authentication_lvl\":\"admin\"}");
#    endif
    }
    //SPIFFS
    //SPIFFS files list and file commands
    void Web_Server::handleFileList() {
        AuthenticationLevel auth_level = is_authenticated();
        if (auth_level == AuthenticationLevel::LEVEL_GUEST) {
            _upload_status = UploadStatusType::NONE;
            _webserver->send(401, "text/plain", "Authentication failed!\n");
            return;
        }

        String path;
        String status = "Ok";
        if (_upload_status == UploadStatusType::FAILED) {
            status         = "Upload failed";
            _upload_status = UploadStatusType::NONE;
        }
        _upload_status = UploadStatusType::NONE;

        //be sure root is correct according authentication
        if (auth_level == AuthenticationLevel::LEVEL_ADMIN) {
            path = "/";
        } else {
            path = "/user";
        }

        //get current path
        if (_webserver->hasArg("path")) {
            path += _webserver->arg("path");
        }

        //to have a clean path
        path.trim();
        path.replace("//", "/");
        if (path[path.length() - 1] != '/') {
            path += "/";
        }

        //check if query need some action
        if (_webserver->hasArg("action")) {
            //delete a file
            if (_webserver->arg("action") == "delete" && _webserver->hasArg("filename")) {
                String filename;
                String shortname = _webserver->arg("filename");
                shortname.replace("/", "");
                filename = path + _webserver->arg("filename");
                filename.replace("//", "/");
                if (!SPIFFS.exists(filename)) {
                    status = shortname + " does not exists!";
                } else {
                    if (SPIFFS.remove(filename)) {
                        status = shortname + " deleted";
                        //what happen if no "/." and no other subfiles ?
                        String ptmp = path;
                        if ((path != "/") && (path[path.length() - 1] = '/')) {
                            ptmp = path.substring(0, path.length() - 1);
                        }

                        File dir        = SPIFFS.open(ptmp);
                        File dircontent = dir.openNextFile();
                        if (!dircontent) {
                            //keep directory alive even empty
                            File r = SPIFFS.open(path + "/.", FILE_WRITE);
                            if (r) {
                                r.close();
                            }
                        }
                    } else {
                        status = "Cannot deleted ";
                        status += shortname;
                    }
                }
            }
            //delete a directory
            if (_webserver->arg("action") == "deletedir" && _webserver->hasArg("filename")) {
                String filename;
                String shortname = _webserver->arg("filename");
                shortname.replace("/", "");
                filename = path + _webserver->arg("filename");
                filename += "/";
                filename.replace("//", "/");
                if (filename != "/") {
                    bool delete_error = false;
                    File dir          = SPIFFS.open(path + shortname);
                    {
                        File file2deleted = dir.openNextFile();
                        while (file2deleted) {
                            String fullpath = file2deleted.name();
                            if (!SPIFFS.remove(fullpath)) {
                                delete_error = true;
                                status       = "Cannot deleted ";
                                status += fullpath;
                            }
                            file2deleted = dir.openNextFile();
                        }
                    }
                    if (!delete_error) {
                        status = shortname;
                        status += " deleted";
                    }
                }
            }

            //create a directory
            if (_webserver->arg("action") == "createdir" && _webserver->hasArg("filename")) {
                String filename;
                filename         = path + _webserver->arg("filename") + "/.";
                String shortname = _webserver->arg("filename");
                shortname.replace("/", "");
                filename.replace("//", "/");
                if (SPIFFS.exists(filename)) {
                    status = shortname + " already exists!";
                } else {
                    File r = SPIFFS.open(filename, FILE_WRITE);
                    if (!r) {
                        status = "Cannot create ";
                        status += shortname;
                    } else {
                        r.close();
                        status = shortname + " created";
                    }
                }
            }
        }

        String ptmp     = path;
        if ((path != "/") && (path[path.length() - 1] = '/')) {
            ptmp = path.substring(0, path.length() - 1);
        }

        File dir = SPIFFS.open(ptmp);
        _webserver->setContentLength(CONTENT_LENGTH_UNKNOWN);
        _webserver->sendHeader("Content-Type", "application/json");
        _webserver->sendHeader("Cache-Control", "no-cache");
        _webserver->send(200);
        _webserver->sendContent("{\"files\":[");
        bool   firstentry = true;
        String subdirlist = "";
        File   fileparsed = dir.openNextFile();
        while (fileparsed) {
            String filename  = fileparsed.name();
            String size      = "";
            bool   addtolist = true;
            // Remove path prefix from name. For root path ('/'), keep the full filename.
            if (path == "/") {
                if (filename.startsWith("/")) {
                    filename = filename.substring(1, filename.length());
                }
            } else {
                filename = filename.substring(path.length(), filename.length());
            }
            //check if file or subfile
            if (filename.indexOf("/") > -1) {
                //Do not rely on "/." to define directory as SPIFFS upload won't create it but directly files
                //and no need to overload SPIFFS if not necessary to create "/." if no need
                //it will reduce SPIFFS available space so limit it to creation
                filename   = filename.substring(0, filename.indexOf("/"));
                String tag = "*";
                tag += filename + "*";
                if (subdirlist.indexOf(tag) > -1 || filename.length() == 0) {  //already in list
                    addtolist = false;                                         //no need to add
                } else {
                    size = "-1";  //it is subfile so display only directory, size will be -1 to describe it is directory
                    if (subdirlist.length() == 0) {
                        subdirlist += "*";
                    }
                    subdirlist += filename + "*";  //add to list
                }
            } else {
                //do not add "." file
                if (!((filename == ".") || (filename == ""))) {
                    size = ESPResponseStream::formatBytes(fileparsed.size());
                } else {
                    addtolist = false;
                }
            }
            if (addtolist) {
                if (!firstentry) {
                    _webserver->sendContent(",");
                } else {
                    firstentry = false;
                }
                _webserver->sendContent("{\"name\":\"");
                _webserver->sendContent(filename);
                _webserver->sendContent("\",\"size\":\"");
                _webserver->sendContent(size);
                _webserver->sendContent("\"}");
            }
            fileparsed = dir.openNextFile();
        }
        _webserver->sendContent("],");
        _webserver->sendContent("\"path\":\"");
        _webserver->sendContent(path);
        _webserver->sendContent("\",\"status\":\"");
        _webserver->sendContent(status);
        _webserver->sendContent("\",");
        size_t totalBytes;
        size_t usedBytes;
        totalBytes = SPIFFS.totalBytes();
        usedBytes  = SPIFFS.usedBytes();
        String totalStr = ESPResponseStream::formatBytes(totalBytes);
        String usedStr  = ESPResponseStream::formatBytes(usedBytes);
        _webserver->sendContent("\"total\":\"");
        _webserver->sendContent(totalStr);
        _webserver->sendContent("\",\"used\":\"");
        _webserver->sendContent(usedStr);
        _webserver->sendContent("\",\"occupation\":\"");
        char occupation[16];
        unsigned occPercent = (totalBytes > 0) ? (unsigned)(100 * usedBytes / totalBytes) : 0U;
        snprintf(occupation, sizeof(occupation), "%u", occPercent);
        _webserver->sendContent(occupation);
        _webserver->sendContent("\"}");
        _webserver->sendContent("");
        path = "";
    }

    //push error code and message to websocket
    void Web_Server::pushError(int code, const char* st, bool web_error, uint16_t timeout) {
        if (_socket_server && st) {
            String s = "ERROR:" + String(code) + ":";
            s += st;
            _socket_server->sendTXT(_id_connection, s);
            if (web_error != 0 && _webserver && _webserver->client().available() > 0) {
                _webserver->send(web_error, "text/xml", st);
            }

            uint32_t t = millis();
            while (millis() - t < timeout) {
                _socket_server->loop();
                delay(10);
            }
        }
    }

    //abort reception of packages
    void Web_Server::cancelUpload() {
        if (_webserver && _webserver->client().available() > 0) {
            HTTPUpload& upload = _webserver->upload();
            upload.status      = UPLOAD_FILE_ABORTED;
            errno              = ECONNABORTED;
            _webserver->client().stop();
            delay(100);
        }
    }

    //SPIFFS files uploader handle
    void Web_Server::SPIFFSFileupload() {
        static String filename;
        static File   fsUploadFile = (File)0;

        //get authentication status
        AuthenticationLevel auth_level = is_authenticated();
        //Guest cannot upload - only admin
        if (auth_level == AuthenticationLevel::LEVEL_GUEST) {
            _upload_status = UploadStatusType::FAILED;
            grbl_send(CLIENT_ALL, "[MSG:Upload rejected]\r\n");
            pushError(ESP_ERROR_AUTHENTICATION, "Upload rejected", 401);
        } else {
            HTTPUpload& upload = _webserver->upload();
            if ((_upload_status != UploadStatusType::FAILED) || (upload.status == UPLOAD_FILE_START)) {
                //Upload start
                //**************
                if (upload.status == UPLOAD_FILE_START) {
                    _upload_status         = UploadStatusType::ONGOING;
                    String upload_filename = upload.filename;
                    if (upload_filename[0] != '/') {
                        filename = "/" + upload_filename;
                    } else {
                        filename = upload.filename;
                    }

                    //according User or Admin the root is different as user is isolate to /user when admin has full access
                    if (auth_level != AuthenticationLevel::LEVEL_ADMIN) {
                        upload_filename = filename;
                        filename        = "/user" + upload_filename;
                    }

                    if (SPIFFS.exists(filename)) {
                        SPIFFS.remove(filename);
                    }
                    if (fsUploadFile) {
                        fsUploadFile.close();
                    }
                    String sizeargname = upload.filename + "S";
                    if (_webserver->hasArg(sizeargname)) {
                        uint32_t filesize  = _webserver->arg(sizeargname).toInt();
                        uint32_t freespace = SPIFFS.totalBytes() - SPIFFS.usedBytes();
                        if (filesize > freespace) {
                            _upload_status = UploadStatusType::FAILED;
                            grbl_send(CLIENT_ALL, "[MSG:Upload error]\r\n");
                            pushError(ESP_ERROR_NOT_ENOUGH_SPACE, "Upload rejected, not enough space");
                        }
                    }

                    if (_upload_status != UploadStatusType::FAILED) {
                        //create file
                        fsUploadFile = SPIFFS.open(filename, FILE_WRITE);
                        //check If creation succeed
                        if (fsUploadFile) {
                            //if yes upload is started
                            _upload_status = UploadStatusType::ONGOING;
                        } else {
                            //if no set cancel flag
                            _upload_status = UploadStatusType::FAILED;
                            grbl_send(CLIENT_ALL, "[MSG:Upload error]\r\n");
                            pushError(ESP_ERROR_FILE_CREATION, "File creation failed");
                        }
                    }
                    //Upload write
                    //**************
                } else if (upload.status == UPLOAD_FILE_WRITE) {
                    vTaskDelay(1 / portTICK_RATE_MS);
                    //check if file is available and no error
                    if (fsUploadFile && _upload_status == UploadStatusType::ONGOING) {
                        //no error so write post date
                        if (upload.currentSize != fsUploadFile.write(upload.buf, upload.currentSize)) {
                            _upload_status = UploadStatusType::FAILED;
                            grbl_send(CLIENT_ALL, "[MSG:Upload error]\r\n");
                            pushError(ESP_ERROR_FILE_WRITE, "File write failed");
                        }
                    } else {
                        //we have a problem set flag UploadStatusType::FAILED
                        _upload_status = UploadStatusType::FAILED;
                        grbl_send(CLIENT_ALL, "[MSG:Upload error]\r\n");
                        pushError(ESP_ERROR_FILE_WRITE, "File write failed");
                    }
                    //Upload end
                    //**************
                } else if (upload.status == UPLOAD_FILE_END) {
                    //check if file is still open
                    if (fsUploadFile) {
                        //close it
                        fsUploadFile.close();
                        //check size
                        String sizeargname = upload.filename + "S";
                        fsUploadFile       = SPIFFS.open(filename, FILE_READ);
                        uint32_t filesize  = fsUploadFile.size();
                        fsUploadFile.close();

                        if (_webserver->hasArg(sizeargname) && _webserver->arg(sizeargname) != String(filesize)) {
                            _upload_status = UploadStatusType::FAILED;
                        }

                        if (_upload_status == UploadStatusType::ONGOING) {
                            _upload_status = UploadStatusType::SUCCESSFUL;
                        } else {
                            grbl_send(CLIENT_ALL, "[MSG:Upload error]\r\n");
                            pushError(ESP_ERROR_UPLOAD, "File upload failed");
                        }
                    } else {
                        //we have a problem set flag UploadStatusType::FAILED
                        _upload_status = UploadStatusType::FAILED;
                        pushError(ESP_ERROR_FILE_CLOSE, "File close failed");
                        grbl_send(CLIENT_ALL, "[MSG:Upload error]\r\n");
                    }
                    //Upload cancelled
                    //**************
                } else {
                    _upload_status = UploadStatusType::FAILED;
                    //pushError(ESP_ERROR_UPLOAD, "File upload failed");
                    return;
                }
            }
        }

        if (_upload_status == UploadStatusType::FAILED) {
            cancelUpload();
            if (SPIFFS.exists(filename)) {
                SPIFFS.remove(filename);
            }
        }
        COMMANDS::wait(0);
    }

    //Web Update handler
    void Web_Server::handleUpdate() {
        AuthenticationLevel auth_level = is_authenticated();
        if (auth_level != AuthenticationLevel::LEVEL_ADMIN) {
            _upload_status = UploadStatusType::NONE;
            _webserver->send(403, "text/plain", "Not allowed, log in first!\n");
            return;
        }

        char jsonfile[32];
        snprintf(jsonfile, sizeof(jsonfile), "{\"status\":\"%d\"}", (int)uint8_t(_upload_status));

        //send status
        _webserver->sendHeader("Cache-Control", "no-cache");
        _webserver->send(200, "application/json", jsonfile);

        //if success restart
        if (_upload_status == UploadStatusType::SUCCESSFUL) {
            COMMANDS::wait(1000);
            COMMANDS::restart_ESP();
        } else {
            _upload_status = UploadStatusType::NONE;
        }
    }

    //File upload for Web update
    void Web_Server::WebUpdateUpload() {
        static size_t   last_upload_update;
        static uint32_t maxSketchSpace = 0;
        static size_t   otaSketchSpace = 0;
        static uint32_t firmwareSize    = 0;

        //only admin can update FW
        if (is_authenticated() != AuthenticationLevel::LEVEL_ADMIN) {
            _upload_status = UploadStatusType::FAILED;
            grbl_send(CLIENT_ALL, "[MSG:Upload rejected]\r\n");
            pushError(ESP_ERROR_AUTHENTICATION, "Upload rejected", 401);
        } else {
            //get current file ID
            HTTPUpload& upload = _webserver->upload();
            if ((_upload_status != UploadStatusType::FAILED) || (upload.status == UPLOAD_FILE_START)) {
                //Upload start
                //**************
                if (upload.status == UPLOAD_FILE_START) {
                    grbl_send(CLIENT_ALL, "[MSG:Update Firmware]\r\n");
                    _upload_status     = UploadStatusType::ONGOING;
                    String sizeargname = upload.filename + "S";
                    if (_webserver->hasArg(sizeargname)) {
                        firmwareSize = _webserver->arg(sizeargname).toInt();
                        maxSketchSpace = firmwareSize;
                    }
                    //check space
                    size_t flashsize = 0;
                    if (esp_ota_get_running_partition()) {
                        const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
                        if (partition) {
                            flashsize = partition->size;
                            otaSketchSpace = partition->size;
                        }
                    }
                    if (flashsize < maxSketchSpace) {
                        pushError(ESP_ERROR_NOT_ENOUGH_SPACE, "Upload rejected, not enough space");
                        _upload_status = UploadStatusType::FAILED;
                        grbl_sendf(CLIENT_ALL, "[MSG:Update cancelled OTA:%u FW:%u]\r\n", uint32_t(otaSketchSpace), uint32_t(firmwareSize));
                    }
                    if (_upload_status != UploadStatusType::FAILED) {
                        last_upload_update = 0;
                        if (!Update.begin()) {  //start with max available size
                            _upload_status = UploadStatusType::FAILED;
                            grbl_sendf(CLIENT_ALL, "[MSG:Update cancelled OTA:%u FW:%u]\r\n", uint32_t(otaSketchSpace), uint32_t(firmwareSize));
                            pushError(ESP_ERROR_NOT_ENOUGH_SPACE, "Upload rejected, not enough space");
                        } else {
                            grbl_send(CLIENT_ALL, "\n[MSG:Update 0%]\r\n");
                        }
                    }
                    //Upload write
                    //**************
                } else if (upload.status == UPLOAD_FILE_WRITE) {
                    vTaskDelay(1 / portTICK_RATE_MS);
                    //check if no error
                    if (_upload_status == UploadStatusType::ONGOING) {
                        if (((100 * upload.totalSize) / maxSketchSpace) != last_upload_update) {
                            if (maxSketchSpace > 0) {
                                last_upload_update = (100 * upload.totalSize) / maxSketchSpace;
                            } else {
                                last_upload_update = upload.totalSize;
                            }

                            String s = "Update ";
                            s += String(last_upload_update);
                            s += "%";
                            grbl_sendf(CLIENT_ALL, "[MSG:%s]\r\n", s.c_str());
                        }
                        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                            _upload_status = UploadStatusType::FAILED;
                            grbl_send(CLIENT_ALL, "[MSG:Update write failed]\r\n");
                            pushError(ESP_ERROR_FILE_WRITE, "File write failed");
                        }
                    }
                    //Upload end
                    //**************
                } else if (upload.status == UPLOAD_FILE_END) {
                    if (Update.end(true)) {  //true to set the size to the current progress
                        //Now Reboot
                        grbl_send(CLIENT_ALL, "[MSG:Update 100%]\r\n");
                        _upload_status = UploadStatusType::SUCCESSFUL;
                    } else {
                        _upload_status = UploadStatusType::FAILED;
                        grbl_send(CLIENT_ALL, "[MSG:Update failed]\r\n");
                        pushError(ESP_ERROR_UPLOAD, "Update upload failed");
                    }
                } else if (upload.status == UPLOAD_FILE_ABORTED) {
                    grbl_send(CLIENT_ALL, "[MSG:Update failed]\r\n");
                    _upload_status = UploadStatusType::FAILED;
                    return;
                }
            }
        }

        if (_upload_status == UploadStatusType::FAILED) {
            cancelUpload();
            Update.end();
        }

        COMMANDS::wait(0);
    }

#    ifdef ENABLE_SD_CARD

    //Function to delete not empty directory on SD card
    bool Web_Server::deleteRecursive(String path) {
        bool result = true;
        File file   = SD.open(path);
        //failed
        if (!file) {
            return false;
        }
        if (!file.isDirectory()) {
            file.close();
            //return if success or not
            return SD.remove(path);
        }
        file.rewindDirectory();
        while (true) {
            File entry = file.openNextFile();
            if (!entry) {
                break;
            }
            String entryPath = entry.name();
            if (entry.isDirectory()) {
                entry.close();
                if (!deleteRecursive(entryPath)) {
                    result = false;
                }
            } else {
                entry.close();
                if (!SD.remove(entryPath)) {
                    result = false;
                    break;
                }
            }
            COMMANDS::wait(0);  //wdtFeed
        }
        file.close();
        return result ? SD.rmdir(path) : false;
    }

    //direct SD files list//////////////////////////////////////////////////
    void Web_Server::handle_direct_SDFileList() {
        //this is only for admin and user
        if (is_authenticated() == AuthenticationLevel::LEVEL_GUEST) {
            _upload_status = UploadStatusType::NONE;
            _webserver->send(401, "application/json", "{\"status\":\"Authentication failed!\"}");
            return;
        }

        String path    = "/";
        String sstatus = "Ok";
        if ((_upload_status == UploadStatusType::FAILED) || (_upload_status == UploadStatusType::FAILED)) {
            sstatus        = "Upload failed";
        }
        _upload_status = UploadStatusType::NONE;
        bool     list_files = true;
        uint64_t totalspace = 0;
        uint64_t usedspace  = 0;
        SDState  state      = get_sd_state(true);
        if (state != SDState::Idle) {
            _webserver->sendHeader("Cache-Control", "no-cache");
            if (state == SDState::NotPresent) {
                _webserver->send(200, "application/json", "{\"status\":\"No SD Card\"}");
            } else {
                _webserver->send(200, "application/json", "{\"status\":\"Busy\"}");
            }
            return;
        }
        set_sd_state(SDState::BusyParsing);

        //get current path
        if (_webserver->hasArg("path")) {
            path += _webserver->arg("path");
        }

        //to have a clean path
        path.trim();
        path.replace("//", "/");
        if (path[path.length() - 1] != '/') {
            path += "/";
        }
        //check if query need some action
        if (_webserver->hasArg("action")) {
            //delete a file
            if (_webserver->arg("action") == "delete" && _webserver->hasArg("filename")) {
                String filename;
                String shortname = _webserver->arg("filename");
                filename         = path + shortname;
                shortname.replace("/", "");
                filename.replace("//", "/");
                if (!SD.exists(filename)) {
                    sstatus = shortname + " does not exist!";
                } else {
                    if (SD.remove(filename)) {
                        sstatus = shortname + " deleted";
                    } else {
                        sstatus = "Cannot deleted ";
                        sstatus += shortname;
                    }
                }
            }
            //delete a directory
            if (_webserver->arg("action") == "deletedir" && _webserver->hasArg("filename")) {
                String filename;
                String shortname = _webserver->arg("filename");
                shortname.replace("/", "");
                filename = path + "/" + shortname;
                filename.replace("//", "/");
                if (filename != "/") {
                    if (!SD.exists(filename)) {
                        sstatus = shortname + " does not exist!";
                    } else {
                        if (!deleteRecursive(filename)) {
                            sstatus = "Error deleting: ";
                            sstatus += shortname;
                        } else {
                            sstatus = shortname;
                            sstatus += " deleted";
                        }
                    }
                } else {
                    sstatus = "Cannot delete root";
                }
            }
            //create a directory
            if (_webserver->arg("action") == "createdir" && _webserver->hasArg("filename")) {
                String filename;
                String shortname = _webserver->arg("filename");
                filename         = path + shortname;
                shortname.replace("/", "");
                filename.replace("//", "/");
                if (SD.exists(filename)) {
                    sstatus = shortname + " already exists!";
                } else {
                    if (!SD.mkdir(filename)) {
                        sstatus = "Cannot create ";
                        sstatus += shortname;
                    } else {
                        sstatus = shortname + " created";
                    }
                }
            }
        }
        //check if no need build file list
        if (_webserver->hasArg("dontlist") && _webserver->arg("dontlist") == "yes") {
            list_files = false;
        }

        if (path != "/") {
            path = path.substring(0, path.length() - 1);
        }
        if (path != "/" && !SD.exists(path)) {
            char notFound[256];
            snprintf(notFound, sizeof(notFound), "{\"status\":\" %s does not exist on SD Card\"}", path.c_str());
            _webserver->send(200, "application/json", notFound);
            SD.end();
            set_sd_state(SDState::Idle);
            return;
        }

        _webserver->setContentLength(CONTENT_LENGTH_UNKNOWN);
        _webserver->sendHeader("Content-Type", "application/json");
        _webserver->sendHeader("Cache-Control", "no-cache");
        _webserver->send(200);

        char   out_chunk[1024];
        size_t out_used = 0;
        auto flush_out = [&]() {
            if (out_used == 0) {
                return;
            }
            out_chunk[out_used] = '\0';
            _webserver->sendContent(out_chunk);
            out_used = 0;
        };
        auto append_out = [&](const char* text) {
            size_t text_len = strlen(text);
            if (text_len == 0) {
                return;
            }

            if (text_len > sizeof(out_chunk) - 1) {
                flush_out();
                _webserver->sendContent(text);
                return;
            }

            if (out_used + text_len > sizeof(out_chunk) - 1) {
                flush_out();
            }

            memcpy(&out_chunk[out_used], text, text_len);
            out_used += text_len;
        };

        append_out("{\"files\":[");

        if (list_files) {
            File dir = SD.open(path);
            if (!dir.isDirectory()) {
                dir.close();
            }
            dir.rewindDirectory();
            File entry = dir.openNextFile();
            int  i     = 0;
            while (entry) {
                COMMANDS::wait(1);
                if (i > 0) {
                    append_out(",");
                }
                const char* entry_name = entry.name();
                const char* base_name  = strrchr(entry_name, '/');
                base_name              = (base_name == NULL) ? entry_name : (base_name + 1);
                char        size_text[24];
                if (entry.isDirectory()) {
                    snprintf(size_text, sizeof(size_text), "-1");
                } else {
                    // files have sizes, directories do not
                    format_bytes_to_text(entry.size(), size_text, sizeof(size_text));
                }

                // Build a full entry line to reduce transient allocations in HTTP chunking.
                char entry_json[640];
                int  entry_len = snprintf(entry_json,
                                         sizeof(entry_json),
                                         "{\"name\":\"%s\",\"shortname\":\"%s\",\"size\":\"%s\",\"datetime\":\"\"}",
                                         base_name,
                                         base_name,
                                         size_text);
                if (entry_len > 0 && entry_len < (int)sizeof(entry_json)) {
                    append_out(entry_json);
                } else {
                    append_out("{\"name\":\"");
                    append_out(base_name);
                    append_out("\",\"shortname\":\"");  //No need here
                    append_out(base_name);
                    append_out("\",\"size\":\"");
                    append_out(size_text);
                    append_out("\",\"datetime\":\"");
                    //TODO - can be done later
                    append_out("\"}");
                }
                i++;
                entry.close();
                entry = dir.openNextFile();
            }
            dir.close();
        }
        char stotalspace[24];
        char susedspace[24];
        //SDCard are in GB or MB but no less
        totalspace  = SD.totalBytes();
        usedspace   = SD.usedBytes();
        format_bytes_to_text(totalspace, stotalspace, sizeof(stotalspace));
        format_bytes_to_text(usedspace + 1, susedspace, sizeof(susedspace));

        uint32_t occupedspace = 1;
        uint32_t usedspace2   = usedspace / (1024 * 1024);
        uint32_t totalspace2  = totalspace / (1024 * 1024);
        if (totalspace2 != 0) {
            occupedspace = (usedspace2 * 100) / totalspace2;
        }
        //minimum if even one byte is used is 1%
        if (occupedspace <= 1) {
            occupedspace = 1;
        }
        append_out("],\"path\":\"");
        append_out(path.c_str());
        append_out("\",\"total\":\"");
        if (totalspace) {
            append_out(stotalspace);
        } else {
            append_out("-1");
        }
        append_out("\",\"used\":\"");
        append_out(susedspace);
        append_out("\",\"occupation\":\"");
        if (totalspace) {
            char occ[16];
            snprintf(occ, sizeof(occ), "%u", (unsigned)occupedspace);
            append_out(occ);
        } else {
            append_out("-1");
        }
        append_out("\",\"mode\":\"direct\",\"status\":\"");
        append_out(sstatus.c_str());
        append_out("\"}");
        flush_out();
        _webserver->sendContent("");
        set_sd_state(SDState::Idle);
        SD.end();
    }

    //SD File upload with direct access to SD///////////////////////////////
    void Web_Server::SDFile_direct_upload() {
        static String filename;
        static File   sdUploadFile;
        //this is only for admin and user
        if (is_authenticated() == AuthenticationLevel::LEVEL_GUEST) {
            _upload_status = UploadStatusType::FAILED;
            _webserver->send(401, "application/json", "{\"status\":\"Authentication failed!\"}");
            pushError(ESP_ERROR_AUTHENTICATION, "Upload rejected", 401);
        } else {
            //retrieve current file id
            HTTPUpload& upload = _webserver->upload();
            if ((_upload_status != UploadStatusType::FAILED) || (upload.status == UPLOAD_FILE_START)) {
                //Upload start
                //**************
                if (upload.status == UPLOAD_FILE_START) {
                    _upload_status = UploadStatusType::ONGOING;
                    filename       = upload.filename;
                    //on SD need to add / if not present
                    if (filename[0] != '/') {
                        filename = "/" + upload.filename;
                    }
                    // Recover stale SD print lock if a previous job was interrupted.
                    SDState sdState = get_sd_state(true);
                    if ((sdState == SDState::BusyPrinting) &&
                        (sys.state != State::Cycle) &&
                        (sys.state != State::Hold) &&
                        (sys.state != State::Jog) &&
                        (sys.state != State::Homing)) {
                        closeFile();
                        sdState = get_sd_state(true);
                    }
                    // Recover stale upload/listing lock if previous HTTP operation ended unexpectedly.
                    if ((sdState == SDState::BusyUploading) || (sdState == SDState::BusyParsing)) {
                        set_sd_state(SDState::Idle);
                        SD.end();
                        sdState = get_sd_state(true);
                    }
                    //check if SD Card is available
                    if (sdState != SDState::Idle) {
                        _upload_status = UploadStatusType::FAILED;
                        String error_text = "Upload cancelled: status=START, file=";
                        error_text += filename;
                        error_text += ", sd_state=";
                        error_text += sd_state_text(sdState);
                        error_text += ", sys_state=";
                        error_text += sys_state_text(sys.state);
                        grbl_sendf(CLIENT_ALL, "[MSG:%s]\r\n", error_text.c_str());
                        pushError(ESP_ERROR_UPLOAD_CANCELLED, error_text.c_str());

                    } else {
                        set_sd_state(SDState::BusyUploading);
                        //delete file on SD Card if already present
                        if (SD.exists(filename)) {
                            SD.remove(filename);
                        }
                        String sizeargname = upload.filename + "S";
                        if (_webserver->hasArg(sizeargname)) {
                            uint32_t filesize  = _webserver->arg(sizeargname).toInt();
                            uint64_t freespace = SD.totalBytes() - SD.usedBytes();
                            if (filesize > freespace) {
                                _upload_status = UploadStatusType::FAILED;
                                grbl_send(CLIENT_ALL, "[MSG:Upload error]\r\n");
                                pushError(ESP_ERROR_NOT_ENOUGH_SPACE, "Upload rejected, not enough space");
                            }
                        }
                        if (_upload_status != UploadStatusType::FAILED) {
                            //Create file for writing
                            sdUploadFile = SD.open(filename, FILE_WRITE);
                            //check if creation succeed
                            if (!sdUploadFile) {
                                //if creation failed
                                _upload_status = UploadStatusType::FAILED;
                                grbl_send(CLIENT_ALL, "[MSG:Upload failed]\r\n");
                                pushError(ESP_ERROR_FILE_CREATION, "File creation failed");
                            }
                            //if creation succeed set flag UploadStatusType::ONGOING
                            else {
                                _upload_status = UploadStatusType::ONGOING;
                            }
                        }
                    }
                    //Upload write
                    //**************
                } else if (upload.status == UPLOAD_FILE_WRITE) {
                    vTaskDelay(1 / portTICK_RATE_MS);
                    if (sdUploadFile && (_upload_status == UploadStatusType::ONGOING) && (get_sd_state(false) == SDState::BusyUploading)) {
                        //no error write post data
                        if (upload.currentSize != sdUploadFile.write(upload.buf, upload.currentSize)) {
                            _upload_status = UploadStatusType::FAILED;
                            grbl_send(CLIENT_ALL, "[MSG:Upload failed]\r\n");
                            pushError(ESP_ERROR_FILE_WRITE, "File write failed");
                        }
                    } else {  //if error set flag UploadStatusType::FAILED
                        _upload_status = UploadStatusType::FAILED;
                        grbl_send(CLIENT_ALL, "[MSG:Upload failed]\r\n");
                        pushError(ESP_ERROR_FILE_WRITE, "File write failed");
                    }
                    //Upload end
                    //**************
                } else if (upload.status == UPLOAD_FILE_END) {
                    //if file is open close it
                    if (sdUploadFile) {
                        sdUploadFile.close();
                        //TODO Check size
                        String sizeargname = upload.filename + "S";
                        if (_webserver->hasArg(sizeargname)) {
                            uint32_t filesize = 0;
                            sdUploadFile      = SD.open(filename, FILE_READ);
                            filesize          = sdUploadFile.size();
                            sdUploadFile.close();
                            if (_webserver->arg(sizeargname) != String(filesize)) {
                                _upload_status = UploadStatusType::FAILED;
                                pushError(ESP_ERROR_UPLOAD, "File upload mismatch");
                                grbl_send(CLIENT_ALL, "[MSG:Upload failed]\r\n");
                            }
                        }
                    } else {
                        _upload_status = UploadStatusType::FAILED;
                        grbl_send(CLIENT_ALL, "[MSG:Upload failed]\r\n");
                        pushError(ESP_ERROR_FILE_CLOSE, "File close failed");
                    }
                    if (_upload_status == UploadStatusType::ONGOING) {
                        _upload_status = UploadStatusType::SUCCESSFUL;
                        set_sd_state(SDState::Idle);
                    } else {
                        _upload_status = UploadStatusType::FAILED;
                        pushError(ESP_ERROR_UPLOAD, "Upload error");
                    }

                } else {  //Upload cancelled
                    _upload_status = UploadStatusType::FAILED;
                    SDState current_state = get_sd_state(false);
                    String  error_text    = "Upload cancelled: status=";
                    error_text += upload_status_text(upload.status);
                    error_text += ", file=";
                    error_text += filename;
                    error_text += ", sd_state=";
                    error_text += sd_state_text(current_state);
                    grbl_sendf(CLIENT_ALL, "%s\r\n", error_text.c_str());
                    pushError(ESP_ERROR_UPLOAD_CANCELLED, error_text.c_str());
                    set_sd_state(SDState::Idle);
                    if (sdUploadFile) {
                        sdUploadFile.close();
                    }
                    SD.end();
                    return;
                }
            }
        }
        if (_upload_status == UploadStatusType::FAILED) {
            cancelUpload();
            if (sdUploadFile) {
                sdUploadFile.close();
            }
            if (SD.exists(filename)) {
                SD.remove(filename);
            }
            set_sd_state(SDState::Idle);
        }
        COMMANDS::wait(0);
    }
#    endif

    void Web_Server::handle() {
        static uint32_t timeout = millis();
        COMMANDS::wait(0);
#    ifdef ENABLE_CAPTIVE_PORTAL
        if (WiFi.getMode() == WIFI_AP) {
            dnsServer.processNextRequest();
        }
#    endif
        if (_webserver) {
            _webserver->handleClient();
        }
        if (_socket_server && _setupdone) {
            _socket_server->loop();
        }
        if ((millis() - timeout) > 10000 && _socket_server && (_socket_server->connectedClients(false) > 0)) {
            // In single-WebUI mode heartbeat includes ACTIVE id for legacy UI behavior.
            if (!webui_secondary_enable || (webui_secondary_enable->get() == 0)) {
                char pingMsg[24];
                snprintf(pingMsg, sizeof(pingMsg), "PING:%ld", _id_connection);
                _socket_server->broadcastTXT(pingMsg);
            }
            timeout = millis();
        }
    }

    void Web_Server::handle_Websocket_Event(uint8_t num, uint8_t type, uint8_t* payload, size_t length) {
        switch (type) {
            case WStype_DISCONNECTED:
                //USE_SERIAL.printf("[%u] Disconnected!\n", num);
                if (_id_connection == num) {
                    _id_connection = -1;
                }
                grbl_send(CLIENT_SERIAL , "WebUI Disconnected!\n");
                break;
            case WStype_CONNECTED: {
                IPAddress ip = _socket_server->remoteIP(num);
                //USE_SERIAL.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
                char wsMsg[32];
                snprintf(wsMsg, sizeof(wsMsg), "CURRENT_ID:%u", num);
                // send message to client
                _id_connection = num;
                _socket_server->sendTXT(_id_connection, wsMsg);
                if (!webui_secondary_enable || (webui_secondary_enable->get() == 0)) {
                    snprintf(wsMsg, sizeof(wsMsg), "ACTIVE_ID:%ld", _id_connection);
                    _socket_server->broadcastTXT(wsMsg);
                }

                grbl_send(CLIENT_SERIAL , "WebUI connected!\n");
            } break;
            case WStype_TEXT:
                //USE_SERIAL.printf("[%u] get Text: %s\n", num, payload);

                // send message to client
                // webSocket.sendTXT(num, "message here");

                // send data to all connected clients
                // webSocket.broadcastTXT("message here");
                break;
            case WStype_BIN:
                //USE_SERIAL.printf("[%u] get binary length: %u\n", num, length);
                //hexdump(payload, length);

                // send message to client
                // webSocket.sendBIN(num, payload, length);
                break;
            default:
                break;
        }
    }

    // The separator that is passed in to this function is always '\n'
    // The string that is returned does not contain the separator
    // The calling code adds back the separator, unless the string is
    // a one-character realtime command.
    String Web_Server::get_Splited_Value(const String& data, char separator, int index) {
        int found      = 0;
        int strIndex[] = { 0, -1 };
        int maxIndex   = data.length() - 1;

        for (int i = 0; i <= maxIndex && found <= index; i++) {
            if (data.charAt(i) == separator || i == maxIndex) {
                found++;
                strIndex[0] = strIndex[1] + 1;
                strIndex[1] = (i == maxIndex) ? i + 1 : i;
            }
        }

        return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
    }

    //helper to extract content type from file extension
    //Check what is the content tye according extension file
    String Web_Server::getContentType(const String& filename) {
        const char* ext = strrchr(filename.c_str(), '.');
        if (ext == NULL) {
            return "application/octet-stream";
        }

        if (strcasecmp(ext, ".htm") == 0 || strcasecmp(ext, ".html") == 0) {
            return "text/html";
        } else if (strcasecmp(ext, ".css") == 0) {
            return "text/css";
        } else if (strcasecmp(ext, ".js") == 0) {
            return "application/javascript";
        } else if (strcasecmp(ext, ".png") == 0) {
            return "image/png";
        } else if (strcasecmp(ext, ".gif") == 0) {
            return "image/gif";
        } else if (strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".jpg") == 0) {
            return "image/jpeg";
        } else if (strcasecmp(ext, ".ico") == 0) {
            return "image/x-icon";
        } else if (strcasecmp(ext, ".xml") == 0) {
            return "text/xml";
        } else if (strcasecmp(ext, ".pdf") == 0) {
            return "application/x-pdf";
        } else if (strcasecmp(ext, ".zip") == 0) {
            return "application/x-zip";
        } else if (strcasecmp(ext, ".gz") == 0) {
            return "application/x-gzip";
        } else if (strcasecmp(ext, ".txt") == 0) {
            return "text/plain";
        }
        return "application/octet-stream";
    }

    //check authentification
    AuthenticationLevel Web_Server::is_authenticated() {
#    ifdef ENABLE_AUTHENTICATION
        if (_webserver->hasHeader("Cookie")) {
            String cookie = _webserver->header("Cookie");
            int    pos    = cookie.indexOf("ESPSESSIONID=");
            if (pos != -1) {
                int       pos2      = cookie.indexOf(";", pos);
                String    sessionID = cookie.substring(pos + strlen("ESPSESSIONID="), pos2);
                IPAddress ip        = _webserver->client().remoteIP();
                //check if cookie can be reset and clean table in same time
                return ResetAuthIP(ip, sessionID.c_str());
            }
        }
        return AuthenticationLevel::LEVEL_GUEST;
#    else
        return AuthenticationLevel::LEVEL_ADMIN;
#    endif
    }

#    ifdef ENABLE_AUTHENTICATION

    //add the information in the linked list if possible
    bool Web_Server::AddAuthIP(AuthenticationIP* item) {
        if (_nb_ip > MAX_AUTH_IP) {
            return false;
        }
        item->_next = _head;
        _head       = item;
        _nb_ip++;
        return true;
    }

    //Session ID based on IP and time using 16 char
    char* Web_Server::create_session_ID() {
        static char sessionID[17];
        //reset SESSIONID
        for (int i = 0; i < 17; i++) {
            sessionID[i] = '\0';
        }
        //get time
        uint32_t now = millis();
        //get remote IP
        IPAddress remoteIP = _webserver->client().remoteIP();
        //generate SESSIONID
        if (0 > sprintf(sessionID,
                        "%02X%02X%02X%02X%02X%02X%02X%02X",
                        remoteIP[0],
                        remoteIP[1],
                        remoteIP[2],
                        remoteIP[3],
                        (uint8_t)((now >> 0) & 0xff),
                        (uint8_t)((now >> 8) & 0xff),
                        (uint8_t)((now >> 16) & 0xff),
                        (uint8_t)((now >> 24) & 0xff))) {
            strcpy(sessionID, "NONE");
        }
        return sessionID;
    }

    bool Web_Server::ClearAuthIP(IPAddress ip, const char* sessionID) {
        AuthenticationIP* current  = _head;
        AuthenticationIP* previous = NULL;
        bool              done     = false;
        while (current) {
            if ((ip == current->ip) && (strcmp(sessionID, current->sessionID) == 0)) {
                //remove
                done = true;
                if (current == _head) {
                    _head = current->_next;
                    _nb_ip--;
                    delete current;
                    current = _head;
                } else {
                    previous->_next = current->_next;
                    _nb_ip--;
                    delete current;
                    current = previous->_next;
                }
            } else {
                previous = current;
                current  = current->_next;
            }
        }
        return done;
    }

    //Get info
    AuthenticationIP* Web_Server::GetAuth(IPAddress ip, const char* sessionID) {
        AuthenticationIP* current = _head;
        //AuthenticationIP * previous = NULL;
        //get time
        //uint32_t now = millis();
        while (current) {
            if (ip == current->ip) {
                if (strcmp(sessionID, current->sessionID) == 0) {
                    //found
                    return current;
                }
            }
            //previous = current;
            current = current->_next;
        }
        return NULL;
    }

    //Review all IP to reset timers
    AuthenticationLevel Web_Server::ResetAuthIP(IPAddress ip, const char* sessionID) {
        AuthenticationIP* current  = _head;
        AuthenticationIP* previous = NULL;
        //get time
        //uint32_t now = millis();
        while (current) {
            if ((millis() - current->last_time) > 360000) {
                //remove
                if (current == _head) {
                    _head = current->_next;
                    _nb_ip--;
                    delete current;
                    current = _head;
                } else {
                    previous->_next = current->_next;
                    _nb_ip--;
                    delete current;
                    current = previous->_next;
                }
            } else {
                if (ip == current->ip && strcmp(sessionID, current->sessionID) == 0) {
                    //reset time
                    current->last_time = millis();
                    return (AuthenticationLevel)current->level;
                }
                previous = current;
                current  = current->_next;
            }
        }
        return AuthenticationLevel::LEVEL_GUEST;
    }
#    endif
}
#endif  // Enable HTTP && ENABLE_WIFI
