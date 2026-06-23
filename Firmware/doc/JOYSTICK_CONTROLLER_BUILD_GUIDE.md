# ESP32 Bluetooth Joystick Controller for MKS-DLC32

## Список компонентов

### Обязательные:
- **ESP32 Development Board** (DevKit v1 / v4) — основной контроллер
- **2-3 axis Joystick Module** (PSP тип или аналогичный) — управление движением
  - Примеры: `KY-023`, `JH-D202`, `JH-D500`
  - Напряжение: 5V или 3.3V в зависимости от модели
  - Выход: 3 аналоговых входа (X, Y, Z) + 1 цифровой (кнопка)

### Опциональные:
- **Rotary Encoder** (с кнопкой) — для управления скоростью подачи
  - Примеры: `KY-040`, `EC11` с кнопкой
  - 3 пина (CLK, DT, SW) + GND + VCC
- **5 Pushbuttons** (тактильные кнопки)
  - 4 кнопки: Home, Zero, Pause, Reset
  - 1 кнопка (опция): Spindle On/Off
- **1 LED Diode + Resistor (220Ω)**
  - Для индикации состояния Bluetooth соединения
- **2 x 10µF Capacitor** (для стабилизации питания)
- **Breadboard / PCB** для монтажа
- **USB кабель Micro-B** для программирования ESP32
- **Провода** (папа-мама, папа-папа, мама-мама)

### Питание:
- **Вариант 1**: USB от компьютера (для разработки)
- **Вариант 2**: Power Bank 5V (для мобильного использования)
- **Вариант 3**: 5V блок питания (в корпусе)

---

## Схема подключения

```
                    ESP32 DevKit v1
                    ================
                    
    USB 5V ──┐
             ├──> GND (multiple pins)
    GND  ────┤
             │
         ┌───┴─────────────────────────────────┐
         │                                     │
         │   5V ─────┬──────────────┬─────┐   │
         │           │              │     │   │
         │          10µF           10µF  LED  │  LED cathode (K)
         │           │              │     │   │
         │          GND            GND    R220Ω
         │                               │   │
    ┌────┴────────────────────────────────┴───┴───────┐
    │                                                   │
    │          JOYSTICK MODULE (2-axis)               │
    │  ┌────────────────────────────────┐             │
    │  │  JOY_5V  ──────────> VCC       │             │
    │  │  JOY_GND ──────────> GND       │             │
    │  │  JOY_X   ──────────> GPIO35    │             │
    │  │  JOY_Y   ──────────> GPIO34    │             │
    │  │  JOY_Z   ──────────> GPIO33    │             │
    │  │  (optional for 3-axis)         │             │
    │  │  JOY_SW  ──────────> GPIO32    │             │
    │  └────────────────────────────────┘             │
    │                                                   │
    │          ROTARY ENCODER (optional)              │
    │  ┌────────────────────────────────┐             │
    │  │  ENC_VCC ──────────> 3.3V      │             │
    │  │  ENC_GND ──────────> GND       │             │
    │  │  ENC_CLK ──────────> GPIO25    │             │
    │  │  ENC_DT  ──────────> GPIO26    │             │
    │  │  ENC_SW  ──────────> GPIO27    │             │
    │  └────────────────────────────────┘             │
    │                                                   │
    │          CONTROL BUTTONS                        │
    │  ┌──> GND                                        │
    │  │                                               │
    │  ├─ BTN_HOME  ──────────> GPIO14                │
    │  ├─ BTN_ZERO  ──────────> GPIO12                │
    │  ├─ BTN_PAUSE ──────────> GPIO13                │
    │  └─ BTN_RESET ──────────> GPIO15                │
    │                                                   │
    │          STATUS LED                             │
    │  ┌──> GPIO32                                     │
    │  │      │                                        │
    │  │     220Ω R                                    │
    │  │      │                                        │
    │  │   ┌──(K)──────┐                              │
    │  │   │ LED (A)   │                              │
    │  └─→─│           │                              │
    │      └──────────→ GND                           │
    │                                                   │
    │    Bluetooth встроен в ESP32 (UART)             │
    │    ↕                                             │
    │    Станок MKS-DLC32 (Bluetooth SPP)             │
    │                                                   │
    └───────────────────────────────────────────────────┘
```

---

## Примеры подключения (реальная распиновка ESP32)

### Вид сверху ESP32 DevKit v1:
```
┌─────────────────────────────────────┐
│  USB          ┌───┐          ANTENNA│
│  ├─ GND       │USB│     ┌───────┐  │
│  ├─ 5V        ├───┤ RST │ ╱════╲│  │
│  │    3.3V    │GND│─────│      │   │
│  │    EN      │23 │    │  CHIP  │  │
│  │    SENSOR_VP│22│     │        │  │
│  │    SENSOR_VN│TX│     │ ESP32  │  │
│  │    34       │RX│     │        │  │
│  │    39       │21│     │        │  │
│  │    36       │GND│─────│      │   │
│  │    4        │19│    │╲════╱ │   │
│  │    2        │18│    └───────┘   │
│  │    15       │5 │                │
│  │    13       │17│                │
│  │    12       │16│                │
│  │    14       │4 │                │
│  │    27       │0 │                │
│  │    26       │35│  ← JOY_X       │
│  │    25       │34│  ← JOY_Y       │
│  │    32       │33│  ← JOY_Z       │
│  │    GND      │GND│               │
│  └─────────────────────────────────┘
```

### Распиновка по категориям:

#### Питание:
- **VIN** (left side) — входное питание 5V
- **3.3V** — выходное питание 3.3V (для джойстика и энкодера)
- **5V** — входное питание 5V (для светодиода)
- **GND** (multiple) — земля (общее минус)

#### Аналоговые входы (ADC):
- **GPIO35** (SENSOR_VP) ← Joystick X
- **GPIO34** (SENSOR_VN) ← Joystick Y  
- **GPIO33** ← Joystick Z (опция для 3-axis)
- **GPIO32** ← LED status

#### Цифровые входы (для кнопок и энкодера):
- **GPIO25** ← Encoder CLK
- **GPIO26** ← Encoder DT
- **GPIO27** ← Encoder SW (кнопка энкодера)
- **GPIO14** ← Button Home
- **GPIO12** ← Button Zero
- **GPIO13** ← Button Pause
- **GPIO15** ← Button Reset

#### Специальные:
- **GPIO1** (TX) — UART Serial (для отладки)
- **GPIO3** (RX) — UART Serial (для отладки)
- **GPIO21, GPIO22** — I2C (зарезервированы)
- **GPIO19, GPIO18** — для SPI Flash (не использовать)

---

## Монтажная схема на макетной плате

### Напряжения и земля (шинопровод):

```
Макетная плата (Breadboard)
┌─ + 5V шина        (слева)
├─ GND шина         (слева)
├─ + 3.3V шина      (справа)
└─ GND шина         (справа)
```

### Рекомендуемый порядок сборки:

1. **Установить ESP32** на макет вертикально (GPIO25-GPIO27 вверху, GND внизу)
2. **Подключить шины питания**:
   - 5V USB → + шина
   - GND USB → GND шины
   - 3.3V ESP32 → + 3.3V шина (опция)
   
3. **Подключить джойстик**:
   - JOY VCC → 5V (или 3.3V в зависимости от модуля)
   - JOY GND → GND
   - JOY X → GPIO35
   - JOY Y → GPIO34
   - JOY Z → GPIO33 (если трехосевой)

4. **Подключить энкодер**:
   - ENC VCC → 3.3V
   - ENC GND → GND
   - ENC CLK → GPIO25
   - ENC DT → GPIO26
   - ENC SW → GPIO27

5. **Подключить кнопки** (каждая кнопка между GPIO и GND):
   - Home → GPIO14 with pull-up resistor
   - Zero → GPIO12
   - Pause → GPIO13
   - Reset → GPIO15

6. **Подключить LED**:
   - LED Anode (длинный вывод) → GPIO32
   - Через resistor 220Ω → LED Cathode (короткий) → GND

7. **Добавить конденсаторы** (на шинах питания):
   - 10µF между 5V и GND
   - 10µF между 3.3V и GND

---

## Программирование ESP32

### 1. Установить Arduino IDE + ESP32 boards:
```bash
# Скачать Arduino IDE: https://www.arduino.cc/en/software
# В IDE: Sketch → Include Library → Manage Libraries
# Найти и установить: "ESP32 by Espressif Systems" (latest version)
```

### 2. Выбрать плату и порт:
```
Tools → Board: → ESP32 Dev Module
Tools → Port: → /dev/cu.usbserial-XXXX (macOS) или COM5 (Windows)
Tools → Upload Speed: → 921600 (или 115200 если нестабильно)
```

### 3. Скопировать код:
- Скопировать содержимое `esp32_joystick_controller.ino` в Arduino IDE
- Или: File → Open → esp32_joystick_controller.ino

### 4. Установить зависимость (BluetoothSerial):
```
Уже встроена в ESP32 core, дополнительной установки не нужно!
```

### 5. Загрузить прошивку:
```
Sketch → Upload (Ctrl+U)
Или кнопка Upload (→)
```

### 6. Проверить работу:
```
Tools → Serial Monitor (Ctrl+Shift+M)
Baud rate: 115200
Вы должны увидеть:
  "Bluetooth Joystick Controller initialized"
  "Looking for device: MKS-DLC32"
```

---

## Запуск и подключение

### На контроллере:
1. Загрузить прошивку в ESP32
2. Питание 5V (USB или Power Bank)
3. Смотреть Serial Monitor — должна говорить "Looking for device: MKS-DLC32"

### На станке:
1. Включить MKS-DLC32, убедиться что Bluetooth включен
2. Проверить, что имя Bluetooth сигнала "MKS-DLC32"

### На мобильном (для отладки):
1. Android: Settings → Bluetooth → найти "MKS-DLC32" контроллер
2. Попарно подключить "MKS-DLC32" станок

### Проверка:
- LED на контроллере должна моргать (поиск), затем гореть (подключено)
- Serial Monitor покажет "Bluetooth connected!"
- Движение джойстика → команды идут на станок
- Станок начинает двигаться!

---

## Тестовая программа (отладка в Serial Monitor)

Когда контроллер запущен, можно отправлять команды через Serial Monitor:

```
help          → Показать список команд
h             → Home (домой)
z             → Zero (обнулить позицию)
p             → Pause (пауза)
r             → Reset (сброс)
?             → Request status
f100          → Set feedrate 100 mm/min
x10           → Move X by 10mm
y-5           → Move Y by -5mm
z2.5          → Move Z by 2.5mm
$H            → Send Grbl command ($H = Home)
```

Пример сеанса отладки:
```
help
Commands:
  h - Home
  z - Zero position
  p - Pause
  r - Reset
  ? - Request status
  f<value> - Set feedrate (e.g., f100)
  x<mm> - Move X axis
  y<mm> - Move Y axis
  z<mm> - Move Z axis
  $ - Enter Grbl command

f150
Feedrate set to: 150.00

x10
-> $J=X10.00F150.00

<- ok
Machine position: 10.000,0.000,0.000
```

---

## Решение проблем

### Bluetooth не подключается:
- Проверить, что MKS-DLC32 включен и имеет Bluetooth включен
- На контроллере проверить Serial Monitor — должно быть "Looking for device: MKS-DLC32"
- Перезагрузить оба устройства

### Джойстик не реагирует:
- Проверить, что джойстик подключен к GPIO35/GPIO34/GPIO33
- Проверить в Serial Monitor, что значения меняются (примерно от 0 до 4095)
- Пример диагностики: добавить в loop() `Serial.printf("Joy: X=%d Y=%d Z=%d\n", analogRead(JOY_X_PIN), analogRead(JOY_Y_PIN), analogRead(JOY_Z_PIN));`

### LED не мигает:
- Проверить полярность LED (длинный вывод = Anode к GPIO32)
- Проверить resistor 220Ω на месте

### Станок получает команды, но не движется:
- Проверить, что станок не в режиме Hold или Alarm
- Отправить `$X` (Unlock)
- Отправить `?` для проверки статуса
- Проверить, что feedrate не ноль

### Контроллер зависает после подключения:
- Возможно, проблема с Bluetooth handshake
- Перезагрузить контроллер (кнопка RST)
- Проверить, что нет других Bluetooth устройств помех

---

## Расширенные возможности (будущие улучшения)

1. **OLED дисплей** (SSD1306, I2C GPIO21/GPIO22):
   - Показывать текущую позицию X/Y/Z
   - Показывать статус станка (Idle/Run/Jog/Hold)
   - Показывать текущую скорость подачи

2. **SD Card** (SPI GPIO18/GPIO19/GPIO23):
   - Сохранять историю движений
   - Запись макросов (G-code последовательностей)

3. **WiFi Control** (дополнительно к Bluetooth):
   - WebUI для управления через браузер

4. **Haptic Feedback** (вибро-мотор):
   - Вибрация при нажатии кнопок
   - Обратная связь при ошибке

5. **IMU Accelerometer** (MPU6050):
   - Наклон контроллера для управления
   - Жесты для специальных команд

---

## Полезные ссылки

- **ESP32 Documentation**: https://docs.espressif.com/projects/esp-idf/
- **Arduino IDE**: https://www.arduino.cc/
- **ESP32 Arduino Core**: https://github.com/espressif/arduino-esp32
- **Grbl Wiki (Jogging)**: https://github.com/grbl/grbl/wiki/Jogging
- **Grbl Commands**: https://github.com/grbl/grbl/wiki/Interfacing-with-Grbl
- **MKS-DLC32 Manual**: https://github.com/makerbase-mks/MKS-DLC32

