# MKS-DLC32 Joystick Controller

Простой **PlatformIO проект** для управления CNC станком MKS-DLC32 через:
- **Bluetooth** (автоматическое подключение)
- **Энкодер** (100 щелчков) — величина движения
- **3 кнопки** — выбор оси X/Y/Z

---

## 📋 Компоненты

### Обязательные:
- **ESP32 DevKit v1** (или аналогичная плата)
- **Rotary Encoder 100 ppr** (с механическим переключателем)
  - Примеры: KY-040, EC11, EC12
  - Хороший выбор: дешевые энкодеры за ~$1-2
- **3x Pushbuttons** (тактильные кнопки, 6mm)
- **3x LED Diodes** (3mm, разных цветов опционально)
- **3x Resistors 220Ω** (для LED)
- **Power Supply** (5V USB или Power Bank)
- **Провода** и **макетная плата**

### Распиновка:

```
ESP32 DevKit v1 Pinout:
┌─────────────────────────┐
│                         │
│  Encoder:               │
│    CLK  → GPIO 25       │
│    DT   → GPIO 26       │
│    SW   → GPIO 27       │
│                         │
│  Buttons:               │
│    X    → GPIO 14       │
│    Y    → GPIO 12       │
│    Z    → GPIO 13       │
│                         │
│  LEDs (with 220Ω R):    │
│    X    → GPIO 32       │
│    Y    → GPIO 33       │
│    Z    → GPIO 15       │
│    Status → GPIO 2      │
│                         │
│  Ground: GND (any)      │
│  Power:  5V (USB)       │
│                         │
└─────────────────────────┘
```

---

## 🔌 Схема подключения

### Энкодер (100 PPR):
```
  CLK ──→ GPIO25
  DT  ──→ GPIO26
  SW  ──→ GPIO27
  +5V ──→ 5V
  GND ──→ GND
```

### Кнопки (каждая):
```
Button ──→ GPIO (14/12/13)
Button ──→ GND
(не нужны resistor pull-ups, используется INPUT_PULLUP в коде)
```

### LED (каждый):
```
LED+ (длинный) ──→ GPIO (32/33/15)
      ↓
    220Ω R
      ↓
LED- (короткий) ──→ GND
```

### Питание:
```
5V USB ──→ VIN или 5V pin
GND    ──→ GND
```

---

## 📦 Установка

### 1. Клонировать / использовать проект:
```bash
# Если это отдельный проект рядом с Firmware:
cd ~/Documents/PlatformIO/Projects/MKS-DLC32-FIRMWARE/JoystickController

# Или открыть в VS Code:
code .
```

### 2. Установить зависимости (уже встроены):
- BluetoothSerial (часть ESP32 Arduino core)
- PlatformIO автоматически скачает нужные версии

### 3. Выбрать COM порт (если нужно):
```bash
# macOS:
ls /dev/cu.usbserial*

# Затем обновить platformio.ini:
monitor_port = /dev/cu.usbserial-XXXX  # ваш порт
```

---

## 🚀 Сборка и прошивка

### Вариант 1: VS Code + PlatformIO extension
```
1. Нажать: Build (✓) или Ctrl+Alt+B
2. Нажать: Upload (→) или Ctrl+Alt+U
3. Нажать: Monitor (⦿) или Ctrl+Alt+I
```

### Вариант 2: Командная строка
```bash
# Сборка
platformio run -e esp32-joystick

# Загрузка в ESP32
platformio run -e esp32-joystick --target upload

# Отладка (Serial Monitor)
platformio device monitor
```

### Вариант 3: Arduino IDE
```bash
# Если привыкли к Arduino IDE
cp src/main.cpp ~/Desktop/joystick_controller.ino
# Затем открыть в Arduino IDE и загрузить
```

---

## 💡 Использование

### Основная работа:

1. **Подключить питание** к ESP32 (через USB или 5V)
2. **Включить станок** MKS-DLC32 с Bluetooth
3. **LED на контроллере**:
   - Мигает медленно → ищет станок
   - Горит постоянно → подключен к станку

4. **Выбрать ось**:
   - Нажать кнопку **X** / **Y** / **Z**
   - Загорится соответствующий LED

5. **Управлять энкодером**:
   - Крутить энкодер **по часовой стрелке** → движение вперед
   - Крутить энкодер **против часовой** → движение назад
   - Каждый щелчок = 1 мм движения

6. **Остановить движение**:
   - Быстро поворачивать энкодер в обе стороны (несколько раз)
   - Или через Serial Monitor: `pause`

### Отладка (Serial Monitor @ 115200 baud):

```
help           → Показать все команды
status         → Текущий статус (подключен ли, выбранная ось)
f150           → Установить скорость подачи 150 мм/мин
home           → Отправить команду Home
zero           → Обнулить позицию (установить текущую как ноль)
pause          → Остановить движение
status?        → Запросить статус станка
x5             → Ручное движение X на +5 мм
y-3            → Ручное движение Y на -3 мм
z0.5           → Ручное движение Z на +0.5 мм
```

Пример сеанса отладки:
```
╔═══════════════════════════════════════╗
║   MKS-DLC32 Joystick Controller v1.0  ║
╚═══════════════════════════════════════╝
✓ Pins initialized
Bluetooth device name: MKS-DLC32
Waiting for connection...

[1000 ms] ✓ Connected to MKS-DLC32!
[1050 ms] → Axis selected: X
[1100 ms] [X] Move +1.0 mm @ 100.0 mm/min
[1150 ms] → Axis selected: Z
[1200 ms] [Z] Move +1.0 mm @ 100.0 mm/min
```

---

## ⚙️ Конфигурация

Все параметры можно менять в `src/main.cpp`:

```cpp
#define ENCODER_PULSES_PER_REV 100  // Щелчки энкодера
#define MOVE_DISTANCE 1.0           // мм на щелчок
#define DEFAULT_FEEDRATE 100        // Стартовая скорость (мм/мин)
#define MAX_FEEDRATE 300            // Максимум
#define MIN_FEEDRATE 50             // Минимум
#define BT_DEVICE_NAME "MKS-DLC32"  // Имя Bluetooth устройства
```

Например, для движения на 0.5 мм за щелчок:
```cpp
#define MOVE_DISTANCE 0.5  // Теперь точнее
```

Или для 2 мм за щелчок:
```cpp
#define MOVE_DISTANCE 2.0  // Более грубое, но быстрое
```

---

## 🔧 Решение проблем

### Bluetooth не подключается:
```
✗ Disconnected from MKS-DLC32
```

**Решение:**
1. Убедиться, что на станке Bluetooth включен
2. На станке включить ENABLE_BLUETOOTH в Config.h и пересобрать
3. Перезагрузить оба устройства
4. Проверить в Serial Monitor: `status`

### Энкодер не реагирует:
1. Проверить распиновку GPIO25, GPIO26, GPIO27
2. Проверить, что энкодер включен (CLK/DT должны быть в контакте)
3. В Serial Monitor запустить диагностику:
   ```
   // Добавить в processEncoderMovement():
   Serial.printf("Encoder count: %ld\n", encoderCount);
   ```

### LED не горит:
1. Проверить полярность LED (длинный вывод к GPIO, короткий к GND)
2. Проверить resistor 220Ω
3. Проверить кабель (может быть поломан)

### Связь прерывается часто:
1. Убедиться, что Bluetooth антенна на ESP32 хорошо контактирует
2. Держать контроллер рядом со станком (< 10 метров)
3. Проверить помехи (Wi-Fi, другие Bluetooth устройства)

---

## 📊 Логирование и отладка

### Включить подробное логирование:
В `src/main.cpp` добавить:
```cpp
#define DEBUG_VERBOSE 1

// В processEncoderMovement():
#ifdef DEBUG_VERBOSE
  Serial.printf("Encoder: %ld steps, distance: %.1f mm\n", steps, moveDistance);
#endif
```

### Просмотр подробного состояния:
```
# Terminal 1: загрузить прошивку
platformio run -e esp32-joystick --target upload

# Terminal 2: мониторить Serial output
platformio device monitor -b 115200 --raw
```

---

## 🚀 Расширения (будущее)

### Готовые идеи:

1. **Режим ускорения** (энкодер быстрее → большее движение)
   ```cpp
   if (steps > 5) moveDistance = 5.0;  // Ускорение
   ```

2. **Шкала скорости энкодером** (комбинированное управление)
   ```cpp
   // Кнопка + энкодер = изменение скорости подачи
   // Энкодер = движение
   ```

3. **OLED дисплей** (I2C GPIO21/GPIO22)
   - Показывать выбранную ось
   - Показывать текущую скорость
   - Показывать статус станка

4. **Сохранение предпочтений** (EEPROM)
   - Последняя использованная скорость
   - Последняя используемая ось

5. **WiFi fallback** (если Bluetooth потеряна)
   - Подключение через WebSocket
   - Управление через WebUI станка

---

## 📝 Правила использования

1. **Всегда проверяйте** что контроллер подключен (LED горит) перед движением
2. **Не обнуляйте позицию** во время работы программы
3. **Помните мертвые зоны** — энкодер может иметь люфт
4. **Регулярно тестируйте** связь: `status?` в Serial Monitor
5. **Не подвергайте контроллер воздействию** воды, пыли, сильных вибраций

---

## 📞 Документация

- **Grbl Wiki**: https://github.com/grbl/grbl/wiki
- **Jogging Protocol**: https://github.com/grbl/grbl/wiki/Jogging
- **ESP32 Docs**: https://docs.espressif.com/
- **MKS-DLC32**: https://github.com/makerbase-mks/MKS-DLC32

---

## 📜 Лицензия

MIT License — используйте свободно, но на свой риск :)

---

## 🐛 Reporting Issues

Если что-то не работает:

1. Запустить `help` в Serial Monitor
2. Проверить распиновку в коде vs вашей сборке
3. Проверить статус подключения: `status`
4. Добавить `Serial.println()` для отладки нужных мест
5. Открыть issue с подробным описанием

---

## 📈 Версия

**v1.0** (2026-06-22)
- ✅ Базовое управление: энкодер + 3 кнопки
- ✅ Автоподключение Bluetooth
- ✅ LED индикация оси + статуса
- ✅ Debug команды в Serial Monitor
- 🔜 v1.1: Ускорение по энкодеру
- 🔜 v1.2: OLED дисплей
- 🔜 v2.0: WiFi fallback + более сложные сценарии

