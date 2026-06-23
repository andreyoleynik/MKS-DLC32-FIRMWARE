# Протокол управления станком через Bluetooth (Joystick Controller)

## Основной принцип

Ваш ESP32 контроллер отправляет команды станку через **Bluetooth Serial (SPP)** на адрес `192.168.4.1:3232` (или имя станка по Bluetooth). Станок принимает команды и отвечает статусом.

---

## 1. Основные команды для движения (Jog)

### Формат команды:
```
$J=X{value}Y{value}Z{value}F{feedrate}
```

### Примеры:
```
$J=X10F100      // Движение на 10 мм по X со скоростью 100 мм/мин
$J=Y-5F50       // Движение на -5 мм по Y со скоростью 50 мм/мин
$J=Z2.5F200     // Движение на +2.5 мм по Z со скоростью 200 мм/мин
$J=X10Y5Z-3F150 // Одновременное движение по всем осям
```

### Параметры:
- `X, Y, Z` — величина перемещения в мм (+ или -)
- `F` — скорость подачи в мм/мин (обязательна, типично 50-500)
- **Минус** перед значением = движение в отрицательном направлении
- **Нету осей** = они не движутся (по умолчанию)

---

## 2. Realtime команды (немедленное выполнение)

Отправляются **одиночными символами**, без Enter. Выполняются немедленно:

| Команда | Действие |
|---------|----------|
| `!` | **Pause/Cancel jog** — остановить текущее движение |
| `~` | **Cycle Start** — начать выполнение G-code или возобновить после паузы |
| `?` | **Status Report** — запросить статус (`<Idle|MPos:0.000,0.000,0.000|FS:0,0>`) |
| `Ctrl+X` | **Soft Reset** — сброс с сохранением текущей позиции |

**Пример:**
```
Отправить: $J=X10F100
Отправить: !         // Остановить движение
Отправить: ?         // Получить статус
```

---

## 3. Системные команды (G-code и Grbl)

Отправляются **со строки** (заканчиваются `\r` или `\n`):

### G-code команды:
```
G0 X10 Y5        // Быстрое движение (rapid) в абсолютные координаты
G1 X10 Y5 F100   // Линейное движение со скоростью 100 мм/мин
G28              // Homing (привод в ноль)
G10 L20 P1 X0 Y0 Z0  // Установить текущую позицию как ноль
```

### Grbl команды:
```
$$                    // Получить все параметры($0, $1, ... $999)
$H                    // Homing (домой)
$X                    // Unlock (разблокировка после ошибки)
$LASTRESET            // Получить причину последнего сброса
$RESETREASON          // Получить код причины сброса
$INFO                 // Получить информацию о станке
```

### Пользовательские команды станка:
```
[ESP110]              // Get radio state (WiFi/BT status)
[ESP111]              // Get current IP
[ESP800]              // Get firmware info
```

---

## 4. Ответы от станка

### Успешное выполнение команды:
```
ok                    // Команда принята и выполнена
```

### Статус-отчет:
```
<Idle|MPos:0.000,0.000,0.000|FS:0,0|WCO:0.000,0.000,0.000>
```
- `Idle` — состояние (Idle, Run, Jog, Hold, Alarm, Door, Check)
- `MPos:X,Y,Z` — текущая позиция в абсолютных координатах
- `FS:F,S` — скорость подачи (F) и скорость шпинделя (S)
- `WCO:X,Y,Z` — смещение рабочей системы координат

### Информационные сообщения:
```
[MSG:Reset]           // Уведомления от системы
[MSG:Grbl 1.1f]
```

### Ошибки:
```
error:1               // Код ошибки (см. Error.h)
```

---

## 5. Рекомендуемая структура протокола для вашего контроллера

### Инициализация:
```cpp
#include <BluetoothSerial.h>

BluetoothSerial SerialBT;
const char* deviceName = "MKS-DLC32";  // Имя станка в Bluetooth

void setup() {
  SerialBT.begin(deviceName);  // Ищет станок по имени в Bluetooth
  Serial.begin(115200);
  delay(1000);
}
```

### Отправка Jog команды:
```cpp
void jogMotion(float x, float y, float z, float feedrate) {
  if (feedrate < 10) feedrate = 10;  // Минимум 10 мм/мин
  
  char cmd[80];
  sprintf(cmd, "$J=X%.1fY%.1fZ%.1fF%.0f\n", x, y, z, feedrate);
  SerialBT.print(cmd);
  
  Serial.printf("Sent: %s", cmd);
}

// Примеры вызовов:
jogMotion(10, 0, 0, 100);    // X вперед
jogMotion(-5, 0, 0, 50);     // X назад
jogMotion(0, 0, 2.5, 150);   // Z вверх
```

### Обработка энкодера + джойстика:
```cpp
// Если энкодер вращается → изменить скорость
void handleEncoder(int clicks) {
  currentFeedrate += clicks * 10;  // +10/-10 на клик
  if (currentFeedrate < 10) currentFeedrate = 10;
  if (currentFeedrate > 500) currentFeedrate = 500;
}

// Если джойстик движется → отправить Jog
void handleJoystick(float joyX, float joyY, float joyZ) {
  // joyX, joyY, joyZ в диапазоне -1.0 ... 1.0
  
  if (abs(joyX) > 0.1 || abs(joyY) > 0.1 || abs(joyZ) > 0.1) {
    float moveX = joyX * 10;   // Максимум 10 мм за раз
    float moveY = joyY * 10;
    float moveZ = joyZ * 10;
    
    jogMotion(moveX, moveY, moveZ, currentFeedrate);
  }
}

// Остановить текущее движение
void stopMotion() {
  SerialBT.write('!');  // Realtime команда
  Serial.println("Motion stopped");
}

// Получить статус
void getStatus() {
  SerialBT.write('?');  // Realtime команда
}
```

### Обработка ответов:
```cpp
void loop() {
  if (SerialBT.available()) {
    String response = "";
    while (SerialBT.available()) {
      char c = SerialBT.read();
      response += c;
      if (c == '\n') break;
    }
    
    Serial.printf("Station replied: %s\n", response.c_str());
    
    if (response.startsWith("<")) {
      // Это статус-отчет
      parseStatus(response);
    } else if (response == "ok\n") {
      // Команда принята
      Serial.println("Command accepted");
    } else if (response.startsWith("error:")) {
      // Ошибка
      Serial.printf("Error: %s\n", response.c_str());
    }
  }
}
```

---

## 6. Рекомендуемая топология вашего контроллера

### Пины ESP32:
```cpp
// ADC входы для аналога джойстика
#define JOY_X_PIN     35   // GPIO35 (вход A0) - Analog
#define JOY_Y_PIN     34   // GPIO34 (вход A1) - Analog
#define JOY_Z_PIN     33   // GPIO33 (вход A2) - Analog

// Энкодер
#define ENCODER_CLK   25   // GPIO25 - CLK энкодера
#define ENCODER_DT    26   // GPIO26 - DT энкодера
#define ENCODER_SW    27   // GPIO27 - кнопка энкодера

// Кнопки управления
#define BTN_HOME      14   // Homing
#define BTN_ZERO      12   // Zero position
#define BTN_PAUSE     13   // Pause/Stop
#define BTN_RESET     15   // Reset

// Дополнительно
#define LED_STATUS    32   // LED состояния соединения
```

### Типичная логика:
```cpp
void setup() {
  // Инициализация джойстика
  analogReadResolution(10);  // 10-bit (0-1023)
  
  // Инициализация энкодера и кнопок
  pinMode(ENCODER_CLK, INPUT);
  pinMode(ENCODER_DT, INPUT);
  pinMode(ENCODER_SW, INPUT_PULLUP);
  attachInterrupt(ENCODER_CLK, onEncoderClk, CHANGE);
  
  // Bluetooth
  SerialBT.begin("MKS-DLC32");
}

void loop() {
  // Читать джойстик
  int rawX = analogRead(JOY_X_PIN);
  int rawY = analogRead(JOY_Y_PIN);
  int rawZ = analogRead(JOY_Z_PIN);
  
  // Нормализовать в диапазон -1.0...1.0
  float joyX = (rawX - 512.0) / 512.0;  // -1.0 ... 1.0
  float joyY = (rawY - 512.0) / 512.0;
  float joyZ = (rawZ - 512.0) / 512.0;
  
  // Мертвая зона (deadzone)
  if (abs(joyX) < 0.1) joyX = 0;
  if (abs(joyY) < 0.1) joyY = 0;
  if (abs(joyZ) < 0.1) joyZ = 0;
  
  // Отправить Jog если есть движение
  if (joyX != 0 || joyY != 0 || joyZ != 0) {
    handleJoystick(joyX, joyY, joyZ);
    delay(100);  // Ограничить частоту отправки команд
  }
  
  // Проверить кнопки
  if (digitalRead(BTN_HOME) == LOW) {
    SerialBT.print("$H\n");
    delay(500);
  }
  
  // Обработать ответы станка
  if (SerialBT.available()) {
    // ... (см. выше)
  }
}
```

---

## 7. Практические советы

1. **Частота отправки команд**: не более 1 команды в 50-100 мс, иначе станок переполнится очередью
2. **Проверка соединения**: периодически отправляйте `?` для проверки статуса
3. **Deadzone**: добавьте мертвую зону для джойстика (±0.1), чтобы не было случайного движения
4. **Feed rate**: стартуйте с 100 мм/мин, затем пользователь может увеличить энкодером
5. **Max distance**: ограничьте одну команду на ~10 мм, чтобы было плавно
6. **Буффер**: Bluetooth может быть нестабилен, отправляйте повторно если нет `ok`

---

## 8. Тестирование

Используйте **Android app "Bluetooth Terminal"** или подключитесь через **Telnet 192.168.4.1:23** чтобы проверить команды:

```
> $J=X10F100
< ok
> ?
< <Jog|MPos:10.000,0.000,0.000|FS:100,0>
> !
< ok
```

Если получаете `error:`, смотрите код ошибки в [Error.h](../Grbl_Esp32/src/Error.h).

---

## Дополнительные материалы

- **Grbl Protocol**: https://github.com/grbl/grbl/wiki/Interfacing-with-Grbl
- **Jogging**: https://github.com/grbl/grbl/wiki/Jogging
- **ESP32 Bluetooth**: https://docs.espressif.com/projects/esp-idf/
