# WebAgent

Лёгкий фоновый агент на C++17, который подключается к удалённому серверу, опрашивает очередь задач, выполняет их локально и возвращает результаты — всё по HTTPS с поддержкой загрузки файлов.

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/build-CMake%203.10%2B-brightgreen.svg)](https://cmake.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## Содержание

- [Обзор](#обзор)
- [Возможности](#возможности)
- [Требования](#требования)
- [Сборка](#сборка)
  - [Linux / macOS](#linux--macos)
  - [Windows (MSVC)](#windows-msvc)
  - [Windows (MinGW)](#windows-mingw)
- [Конфигурация](#конфигурация)
  - [Поля config.json](#поля-configjson)
  - [Переменные окружения](#переменные-окружения)
- [Запуск](#запуск)
- [Типы задач](#типы-задач)
- [Формат логов](#формат-логов)
- [Структура проекта](#структура-проекта)
- [Решение проблем](#решение-проблем)

---

## Обзор

WebAgent регистрируется на сервере по уникальному UID, после чего входит в бесконечный цикл опроса. Когда сервер добавляет задачу в очередь для этого агента, агент забирает её, обрабатывает в воркер-потоке и отправляет результат обратно — включая опциональные файловые вложения.

```
┌───────────┐   POST /wa_reg/    ┌────────────┐
│           │ ────────────────►  │            │
│   Агент   │   POST /wa_task/   │   Сервер   │
│           │ ◄──────────────►   │            │
│           │   POST /wa_result/ │            │
│           │ ────────────────►  │            │
└───────────┘                    └────────────┘
```

---

## Возможности

| Функция | Описание |
|---|---|
| **Пул потоков** | 4 воркера по умолчанию (настраивается через `AGENT_WORKERS`). Задачи выполняются параллельно, не блокируя цикл опроса. |
| **Адаптивный backoff** | При сетевых ошибках интервал опроса удваивается до `backoff_max_sec`. После восстановления связи мгновенно возвращается к норме. |
| **Структурированные логи** | Каждая строка лога содержит временну́ю метку (точность мс), ID потока, код задачи и ID сессии. |
| **Runtime-конфигурация** | Все параметры берутся из `config.json` и/или переменных окружения. Никаких захардкоженных значений. |
| **Корректное завершение** | `SIGINT` / `SIGTERM` дожидаются завершения всех активных задач перед выходом. |
| **Multipart-загрузка** | Результаты и файловые вложения отправляются как `multipart/form-data` через MIME API libcurl. |
| **Кроссплатформенность** | Собирается на Linux, macOS и Windows (MSVC / MinGW). |

---

## Требования

| Зависимость | Версия |
|---|---|
| Компилятор C++ | GCC 8+, Clang 7+, MSVC 19.14+ |
| CMake | 3.10+ |
| libcurl | 7.56+ (MIME API) |

### Установка libcurl

**Ubuntu / Debian**
```bash
sudo apt install libcurl4-openssl-dev
```

**macOS (Homebrew)**
```bash
brew install curl
```

**Windows** — скачайте готовые бинарники с [curl.se/windows](https://curl.se/windows/) или установите через vcpkg:
```powershell
vcpkg install curl:x64-windows
```

---

## Сборка

### Linux / macOS

```bash
git clone https://github.com/yourname/WEB-AGENT.git](https://github.com/essentomori/WEB-AGENT
cd WEB-AGENT
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Бинарник `WebAgent` появится в папке `build/`.

#### Release-сборка
```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Windows (MSVC)

```powershell
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

### Windows (MinGW)

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCURL_ROOT="C:/curl"
mingw32-make
```

---

## Конфигурация

При запуске агент читает `assets/config.json` (или путь, переданный первым аргументом командной строки). Переменные окружения имеют наивысший приоритет и перекрывают значения из файла.

### Поля config.json

```jsonc
{
    "base_url":            "https://your-server:9999/app/webagent1/api",  // обязательно
    "uid":                 "agent-42",                                     // обязательно
    "description":         "web-agent",
    "poll_interval_sec":   60,
    "backoff_max_sec":     300,
    "max_reg_retries":     3,
    "request_timeout_sec": 30,
    "connect_timeout_sec": 10,
    "task_dir":            "tasks",
    "result_dir":          "results",
    "log_dir":             "logs"
}
```

| Поле | Тип | По умолчанию | Описание |
|---|---|---|---|
| `base_url` | string | — | **Обязательно.** Корневой URL серверного API. |
| `uid` | string | — | **Обязательно.** Уникальный идентификатор агента. |
| `description` | string | `"web-agent"` | Метка, отправляемая в каждом запросе. |
| `poll_interval_sec` | int | `60` | Нормальный интервал опроса в секундах. |
| `backoff_max_sec` | int | `300` | Верхняя граница интервала при адаптивном backoff. |
| `max_reg_retries` | int | `3` | Максимальное число попыток регистрации. |
| `request_timeout_sec` | long | `30` | Таймаут передачи libcurl (`CURLOPT_TIMEOUT`). |
| `connect_timeout_sec` | long | `10` | Таймаут подключения libcurl (`CURLOPT_CONNECTTIMEOUT`). |
| `task_dir` | string | `"tasks"` | Зарезервировано для локального хранения задач. |
| `result_dir` | string | `"results"` | Зарезервировано для локального кэша результатов. |
| `log_dir` | string | `"logs"` | Каталог для лог-файлов (при включённой записи в файл). |

> **Примечание:** поле `access_code` записывается в `config.json` автоматически после успешной регистрации. Не задавайте его вручную, если у вас нет заранее выданного токена.

### Переменные окружения

Переменные, установленные в оболочке, перекрывают соответствующие поля `config.json`.

| Переменная | Перекрывает |
|---|---|
| `AGENT_BASE_URL` | `base_url` |
| `AGENT_UID` | `uid` |
| `AGENT_POLL_INTERVAL` | `poll_interval_sec` |
| `AGENT_WORKERS` | Размер пула потоков (по умолчанию `4`) |

---

## Запуск

```bash
# Путь к конфигу по умолчанию (assets/config.json относительно CWD)
./WebAgent

# Явный путь к конфигу
./WebAgent /etc/webagent/config.json
```

Для корректного завершения нажмите `Ctrl+C` или выполните `kill -TERM <pid>`. Агент дождётся окончания всех активных задач перед выходом.

---

## Типы задач

Сервер может отправить четыре встроенных типа задач. Агент выбирает нужный обработчик по полю `task_code`.

### `CONF` — Изменить параметр конфигурации

Записывает пару ключ-значение в `config.json`. Если изменяется `poll_interval_sec`, изменение вступает в силу немедленно без перезапуска.

**Поле options:**
```json
{"key": "poll_interval_sec", "value": "30"}
```

### `FILE` — Загрузить файл на сервер

Читает локальный файл и отправляет его как multipart-вложение.

**Поле options:**
```json
{"filename": "assets/config.json"}
```

Если `filename` не указан или равен `"config.json"`, используется активный файл конфигурации.

### `TASK` — Выполнить команду оболочки

Запускает произвольную команду оболочки и возвращает stdout (с усечением до 2000 символов).

**Поле options:**
```json
{"command": "uname -a"}
```

### `TIMEOUT` — Изменить интервал опроса

Обновляет `poll_interval_sec` в памяти немедленно. Эквивалентно отправке задачи `CONF` для этого ключа, но без записи на диск.

**Поле options:**
```json
{"interval": "15"}
```

---

## Формат логов

Каждая строка имеет следующий формат:

```
2024-05-18 14:32:01.247 [INFO   ] [T:3a1f] [task=TASK] [session=39493...] [TASK] exec: uname -a
```

| Поле | Описание |
|---|---|
| `2024-05-18 14:32:01.247` | Временна́я метка с точностью до миллисекунд |
| `[INFO   ]` | Уровень лога: `DEBUG`, `INFO   `, `WARNING`, `ERROR  `, `CRIT   ` |
| `[T:3a1f]` | Короткий ID потока (4 hex-символа) |
| `[task=TASK]` | Код задачи (отсутствует в не связанных с задачей сообщениях) |
| `[session=…]` | ID сессии (отсутствует в не связанных с задачей сообщениях) |

---

## Структура проекта

```
WEB-AGENT/
├── assets/
│   └── config.json          # Конфигурация времени выполнения
├── include/
│   ├── agent.h              # Все типы, пространства имён и объявления публичного API
│   └── thread_pool.h        # SafeQueue<T> + ThreadPool (header-only)
├── src/
│   ├── main.cpp             # Точка входа: обработчики сигналов, инициализация curl, запуск
│   ├── polling.cpp          # Загрузчик конфига, реализация Logger, обработчики задач, цикл опроса
│   └── result_sender.cpp    # send_result() через libcurl MIME multipart
└── CMakeLists.txt
```

---

## Решение проблем

**`Config file missing or empty`**
Агент не может найти `config.json`. Убедитесь, что бинарник запускается из корня проекта, или укажите путь к конфигу явно:
```bash
./WebAgent /absolute/path/to/config.json
```

**`Config validation: base_url is required`**
В конфиге отсутствует `base_url`. Добавьте его или задайте переменную окружения `AGENT_BASE_URL`.

**`Registration ok but access_code absent`**
Сервер вернул HTTP 200 с `code_response: 0`, но без `access_code`. Проверьте серверные логи.

**`No saved access_code found after -3`**
Сервер сообщает, что агент уже зарегистрирован (`code_response: -3`), но в `config.json` нет `access_code`. Удалите запись об агенте на стороне сервера и дайте агенту зарегистрироваться заново, или добавьте правильный код в `config.json` вручную.

**`curl error: SSL certificate problem`**
Сервер использует самоподписанный сертификат. Агент в данный момент отключает проверку сертификата (`CURLOPT_SSL_VERIFYPEER = 0`). Для production-окружения укажите CA bundle в настройках curl в `main.cpp`.

**Агент сразу завершает работу после запуска**
Проверьте stderr на наличие сообщений `[CRIT]`. Чаще всего причина — отсутствующий или некорректный `config.json`.

**Задачи накапливаются в очереди, но не выполняются**
Увеличьте количество воркеров: задайте `export AGENT_WORKERS=8` перед запуском агента.
