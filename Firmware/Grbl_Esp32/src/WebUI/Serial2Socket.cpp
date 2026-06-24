/*
  Serial2Socket.cpp -  serial 2 socket functions class

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

#    include "Serial2Socket.h"
#    include "WebServer.h"
#    include <WebSocketsServer.h>
#    include <WiFi.h>

namespace WebUI {
    Serial_2_Socket Serial2Socket;

    // TX-путь зовётся из нескольких задач (protocol loop через grbl_send,
    // clientCheckTask из realtime-`?`), broadcastBIN не реентерабелен, а сам
    // WebSocketsServer удаляется при выключении/рестарте Wi-Fi. Поэтому буфер,
    // отправка и attach/detach сериализованы одним мьютексом. При заполнении
    // буфера write() флашит сам (под локом) — большие ответы ($$ для вкладки
    // настроек WebUI) не теряются. broadcastBIN из колбэков зовёт только
    // grbl_send(CLIENT_SERIAL) -> Uart0, рекурсивного захвата нет.
    static SemaphoreHandle_t s2s_tx_mux = xSemaphoreCreateMutex();

    Serial_2_Socket::Serial_2_Socket() {
        _web_socket   = NULL;
        _TXbufferSize = 0;
        _RXbufferSize = 0;
        _RXbufferpos  = 0;
    }

    void Serial_2_Socket::begin(long speed) {
        _TXbufferSize = 0;
        _RXbufferSize = 0;
        _RXbufferpos  = 0;
    }

    void Serial_2_Socket::end() {
        _TXbufferSize = 0;
        _RXbufferSize = 0;
        _RXbufferpos  = 0;
    }

    long Serial_2_Socket::baudRate() { return 0; }

    bool Serial_2_Socket::attachWS(WebSocketsServer* web_socket) {
        if (web_socket) {
            xSemaphoreTake(s2s_tx_mux, portMAX_DELAY);
            _web_socket   = web_socket;
            _TXbufferSize = 0;
            xSemaphoreGive(s2s_tx_mux);
            return true;
        }
        return false;
    }

    bool Serial_2_Socket::detachWS() {
        xSemaphoreTake(s2s_tx_mux, portMAX_DELAY);
        _web_socket   = NULL;
        _TXbufferSize = 0;
        xSemaphoreGive(s2s_tx_mux);
        return true;
    }

    Serial_2_Socket::operator bool() const { return true; }

    int Serial_2_Socket::available() { return _RXbufferSize; }

    size_t Serial_2_Socket::write(uint8_t c) { return write(&c, 1); }

    size_t Serial_2_Socket::write(const uint8_t* buffer, size_t size) {
        if (buffer == NULL) {
            log_i("[SOCKET]No buffer");
            return 0;
        }

#    if defined(ENABLE_SERIAL2SOCKET_OUT)
        xSemaphoreTake(s2s_tx_mux, portMAX_DELAY);
        // Проверка указателя — под локом: detachWS() обнуляет его до того,
        // как Web_Server::end() удалит сервер (иначе use-after-free).
        if (!_web_socket) {
            xSemaphoreGive(s2s_tx_mux);
            log_i("[SOCKET]No socket");
            return 0;
        }
        if (_TXbufferSize == 0) {
            _lastflush = millis();
        }
        for (size_t i = 0; i < size; i++) {
            if (_TXbufferSize >= TXBUFFERSIZE) {
                flush_locked();
            }
            _TXbuffer[_TXbufferSize++] = buffer[i];
        }
        xSemaphoreGive(s2s_tx_mux);
#    endif
        return size;
    }

    int Serial_2_Socket::peek(void) {
        if (_RXbufferSize > 0) {
            return _RXbuffer[_RXbufferpos];
        } else {
            return -1;
        }
    }

    bool Serial_2_Socket::push(const char* data) {
#    if defined(ENABLE_SERIAL2SOCKET_IN)
        int data_size = strlen(data);
        if ((data_size + _RXbufferSize) <= RXBUFFERSIZE) {
            int current = _RXbufferpos + _RXbufferSize;
            if (current > RXBUFFERSIZE) {
                current = current - RXBUFFERSIZE;
            }

            for (int i = 0; i < data_size; i++) {
                if (current > (RXBUFFERSIZE - 1)) {
                    current = 0;
                }
                _RXbuffer[current] = data[i];
                current++;
            }

            _RXbufferSize += strlen(data);
            return true;
        }
        return false;
#    else
        return true;
#    endif
    }

    int Serial_2_Socket::read(void) {
        if (_RXbufferSize > 0) {
            int v = _RXbuffer[_RXbufferpos];
            _RXbufferpos++;

            if (_RXbufferpos > (RXBUFFERSIZE - 1)) {
                _RXbufferpos = 0;
            }
            _RXbufferSize--;
            return v;
        } else {
            return -1;
        }
    }

    void Serial_2_Socket::handle_flush() {
        // Чтение без лока безвредно: точную проверку делает flush() под мьютексом.
        // Переполнение сюда не доходит — write() флашит сам при заполнении.
        if (_TXbufferSize > 0 && ((millis() - _lastflush) > FLUSHTIMEOUT)) {
            log_i("[SOCKET]need flush, buffer size %d", _TXbufferSize);
            flush();
        }
    }

    void Serial_2_Socket::flush(void) {
        xSemaphoreTake(s2s_tx_mux, portMAX_DELAY);
        flush_locked();
        xSemaphoreGive(s2s_tx_mux);
    }

    void Serial_2_Socket::flush_locked() {
        if (_TXbufferSize && _web_socket) {
            _web_socket->broadcastBIN(_TXbuffer, _TXbufferSize);
        }
        _TXbufferSize = 0;
        _lastflush    = millis();
    }

    Serial_2_Socket::~Serial_2_Socket() {
        if (_web_socket) {
            detachWS();
        }
        _TXbufferSize = 0;
        _RXbufferSize = 0;
        _RXbufferpos  = 0;
    }
}
#endif  // ENABLE_WIFI
