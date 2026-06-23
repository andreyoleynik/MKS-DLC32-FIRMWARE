# Быстрый старт

## Открыть проект в VS Code

### Опция 1: Из папки JoystickController
```bash
cd ~/Documents/PlatformIO/Projects/MKS-DLC32-FIRMWARE/JoystickController
code .
```

### Опция 2: Многопроектная среда (рекомендуется)
```bash
# Открыть родительскую папку (содержит оба проекта)
cd ~/Documents/PlatformIO/Projects/MKS-DLC32-FIRMWARE
code .
```

Затем в VS Code:
- **Левая панель** → PlatformIO → Развернуть список проектов
- Выбрать **esp32-joystick** (JoystickController)

---

## Пошаговая сборка

### Шаг 1: Подключить ESP32 через USB

### Шаг 2: Выбрать среду в VS Code
```
VS Code левая панель (PlatformIO)
  ├── Build
  ├── Upload
  ├── Monitor
  └── Clean
```

Кликнуть на **Build** (первый раз долго, ~30 сек):
```
[============================================================================] 100% 
Environment esp32-joystick: Everything is Ok
```

### Шаг 3: Загрузить в ESP32
Кликнуть на **Upload**:
```
Uploading .pio/build/esp32-joystick/firmware.bin v4.5.1
Chip is ESP32-D0WDQ6 (revision 1)
Configuring flash size...
Compressed 236864 bytes to 137906...
Writing at 0x00010000... (60%)
```

Ждать ~10 сек, пока загрузится.

### Шаг 4: Запустить Serial Monitor
Кликнуть на **Monitor**:
```
╔═══════════════════════════════════════╗
║   MKS-DLC32 Joystick Controller v1.0  ║
╚═══════════════════════════════════════╝
✓ Pins initialized
Bluetooth device name: MKS-DLC32
Waiting for connection...
```

---

## Включить расширенные функции

В файле `include/features.h` раскомментировать нужную:

```cpp
// В features.h:
#define FEATURE_SPEED_CONTROL_WITH_BUTTON  ← раскомментировать
```

Затем в `src/main.cpp` добавить:
```cpp
#include "features.h"  // Добавить в начало

// В processEncoderMovement() заменить на:
processEncoderWithButton();  // Вместо обычного processEncoderMovement()
```

Пересобрать: **Build** → **Upload**

---

## Диагностика проблем

Если что-то не работает, запустить диагностику в Serial Monitor:

```
> help          # Показать все команды
> status        # Текущий статус
> status?       # Запросить статус станка
> home          # Тест: отправить Home
```

### Результат:
```
Connected: YES
Current axis: X
Feedrate: 100.0 mm/min
→ Home command sent
<Idle|MPos:0.000,0.000,0.000|...>
✓ Machine responded!
```

Если вы видите:
```
Connected: NO
```
→ Bluetooth не подключен. Проверить на станке Bluetooth.

---

## Вернуться к разработке Firmware станка

```bash
# Из папки JoystickController
cd ../Firmware

# Или прямо
code ~/Documents/PlatformIO/Projects/MKS-DLC32-FIRMWARE/Firmware
```

Выбрать в PlatformIO: `mks_dlc32_v2_1` вместо `esp32-joystick`

---

## Полезные горячие клавиши (VS Code + PlatformIO)

| Комбинация | Действие |
|-----------|----------|
| `Ctrl+Alt+B` | Build |
| `Ctrl+Alt+U` | Upload |
| `Ctrl+Alt+I` | Monitor |
| `Ctrl+Alt+C` | Clean |
| `Ctrl+Alt+S` | Serial Port Select |

---

## Структура проекта

```
JoystickController/
├── platformio.ini          ← Конфигурация PlatformIO
├── README.md               ← Полное руководство
├── QUICKSTART.md           ← Этот файл
│
├── src/
│   └── main.cpp           ← Основной код контроллера
│
├── include/
│   └── features.h         ← Опциональные расширения
│
└── .pio/
    └── (сгенерированные файлы сборки)
```

---

## Следующие шаги

1. ✅ Собрать контроллер (железо)
2. ✅ Загрузить прошивку
3. ✅ Подключить к станку
4. ➡️ Тестировать основные функции
5. ➡️ Добавить расширения (если нужны)
6. ➡️ Интегрировать в вашу рабочий процесс

