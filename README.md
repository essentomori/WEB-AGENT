# WebAgent-LAB

Учебный проект — агент удалённого управления (Remote Management Agent).
Агент регистрируется на сервере, получает задания и отправляет результат обратно с файлами.

---

## Команда

| Участник | Ветка | Роль |
|---|---|---|
| @essentomori | `Kovalev` | Team Lead |
| @t9tu0 | `Smirnov` | Разработка (`polling.cpp`), (`main.cpp`, `agent.h`) |
| @miroslav_pug | `Pugovkin` | Разработка (`result_sender.cpp`) |
| @KrlKot | — | Проектировщик |
| @XXI_Primarch | `Naumov` | Техписатель (`README.md`) |
| @ItQ0n | `Gomonov` | Тестировщик |
---

## Архитектура

```
Сервер (xdev.arkcom.ru:9999)
        │  HTTPS
        ▼
   ┌─────────────────────────────────────┐
   │            agent.h                  │  общие типы, конфиг, объявления
   ├──────────────┬──────────────────────┤
   │  main.cpp    │  polling.cpp         │  result_sender.cpp
   │  @essentomori│  @t9tu0              │  @miroslav_pug
   │  регистрация │  цикл опроса         │  отправка результата
   │  /wa_reg/    │  /wa_task/           │  /wa_result/
   └──────────────┴──────────────────────┘
```

**Поток работы:**
1. Агент регистрируется → получает `access_code`
2. Каждые N секунд опрашивает сервер на наличие задания
3. При получении задания (`status: RUN`) — выполняет его
4. Отправляет результат с файлами на сервер
5. При неверном `access_code` — автоматически перерегистрируется

---

## Требования

- Linux / WSL (Ubuntu 20.04+)
- g++ с поддержкой C++17
- libcurl с OpenSSL

```bash
sudo apt install build-essential libcurl4-openssl-dev
```

---

## Сборка

### Через Make (быстро)

```bash
git clone https://github.com/essentomori/WebAgent-LAB.git
cd WebAgent-LAB
mkdir build && cd build
cmake ..
make
```

### Вручную (одной командой)

```bash
g++ -std=c++17 -O2 -o web_agent \
    src/main.cpp src/polling.cpp src/result_sender.cpp \
    -lcurl
```

---

## Настройка

Откройте `src/agent.h` и отредактируйте блок `Config`:

```cpp
namespace Config {
    const std::string BASE_URL   = "https://xdev.arkcom.ru:9999/app/webagent1/api";
    const std::string AGENT_UID  = "007";          // ← ваш UID
    const std::string AGENT_DESC = "web-agent";

    // Если агент уже зарегистрирован — вставьте access_code сюда:
    const std::string HARDCODED_ACCESS_CODE = "";  // ← вставить код

    const int POLL_INTERVAL_SEC = 5;   // интервал опроса в секундах
    const int MAX_REG_RETRIES   = 3;   // попыток при ошибке доступа
}
```

---

## Запуск

```bash
cd build
./web_agent
```

Пример вывода:

```
2026-03-17 15:00:00 [INFO] === WebAgent запускается ===
2026-03-17 15:00:00 [INFO] Регистрация агента UID=007 ...
2026-03-17 15:00:01 [INFO] Регистрация успешна. access_code=594807-1ddb-...
2026-03-17 15:00:01 [INFO] Цикл опроса запущен. Интервал: 5 сек.
2026-03-17 15:00:06 [INFO] Заданий нет (WAIT). Следующий опрос через 5 сек.
2026-03-17 15:00:11 [INFO] Получено задание: task_code=CONF session_id=bvLeD2gv-...
2026-03-17 15:00:11 [INFO] [CONF] Задание выполнено. Отправляем результат...
2026-03-17 15:00:12 [INFO] Результат принят сервером. OK.
```

---

## Структура проекта

```
WebAgent-LAB/
├── src/
│   ├── agent.h              # общий заголовок
│   ├── main.cpp             # точка входа, регистрация
│   ├── polling.cpp          # цикл опроса заданий
│   └── result_sender.cpp    # отправка результата
├── tests/
│   ├── test_register.cpp    # тесты регистрации
│   ├── test_polling.cpp     # тесты опроса
│   └── test_result.cpp      # тесты отправки результата
├── docs/
│   ├── API.md               # описание API эндпоинтов
│   └── ARCHITECTURE.md      # архитектурные схемы
├── .github/
│   └── workflows/
│       └── build.yml        # CI сборка
├── CMakeLists.txt
├── .gitignore
└── README.md
```

---

## API

| Эндпоинт | Метод | Описание |
|---|---|---|
| `/api/wa_reg/` | POST | Регистрация агента |
| `/api/wa_task/` | POST | Запрос задания |
| `/api/wa_result/` | POST multipart | Отправка результата с файлами |

Подробнее — в [docs/API.md](docs/API.md).

---

## Тесты

```bash
cd build
ctest --verbose
```

---

## Ветки

```
main           ← стабильный код, только через PR
  └── dev      ← интеграционная
        ├── Kovalev      (@essentomori)
        ├── Smirnov      (@t9tu0)
        ├── Pugovkin     (@miroslav_pug)
        └── Naumov       (@XXI_Primarch)
        └── Gomonov      (@ItQ0n)
```
