#pragma once

/*
  TelnetServer.h -  telnet service functions class

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

#include "../Config.h"

class WiFiServer;
class WiFiClient;

namespace WebUI {
    class Telnet_Server {

        static const int TELNETRXBUFFERSIZE = 1200;
        static const int FLUSHTIMEOUT       = 500;

    public:
        Telnet_Server();

        bool   begin();
        void   end();

        void handle();

        size_t write(const uint8_t* buffer, size_t size);
        int    read(void);
        int    peek(void);
        int    available();
        int    get_rx_buffer_available();
        bool   push(uint8_t data);
        bool   push(const uint8_t* data, int datasize);

        static uint16_t port() { return _port; }

        ~Telnet_Server();

    private:
        static WiFiServer* _telnetserver;

        bool        _setupdone;
        WiFiClient  _telnetClient;

#ifdef ENABLE_TELNET_WELCOME_MSG
        IPAddress _telnetClientIP;
#endif
        static uint16_t _port;

        uint32_t _lastflush;
        uint8_t  _RXbuffer[TELNETRXBUFFERSIZE];
        uint16_t _RXbufferSize;
        uint16_t _RXbufferpos;

        uint8_t _client_index;

        bool is_connected()
        {
            return _telnetClient.connected();
        }

        void setup_client(WiFiClient& client)
        {
            _telnetClientIP = IPAddress(0, 0, 0, 0);

            if(_telnetClient.connected())
                _telnetClient.stop();
            
            _telnetClient = client;
            _telnetClient.setNoDelay(true);
        }

    public:
        static void begin_all();
        static void handle_all();
        static void end_all();

        static void write(uint8_t client, const uint8_t* buffer, size_t size);
        static bool read(char* code, uint8_t* client);
        static bool attach_client(WiFiClient& client);

        static int get_rx_buffer_available(uint8_t client);

        static void _handle_clients();
    };
    
    extern Telnet_Server telnet_server[TELNET_CLIENTS_TOTAL];
}