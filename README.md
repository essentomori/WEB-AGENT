# WebAgent

Агент удалённого управления на C++17. Подключается к серверу, получает задания и возвращает результат. Архитектура аналогична системам класса RMM (Remote Monitoring & Management).

**Сервер:** `https://xdev.arkcom.ru:9999`

---

## Содержание

- [Быстрый старт](#быстрый-старт)
- [Архитектура](#архитектура)
- [Структура проекта](#структура-проекта)
- [Конфигурация](#конфигурация)
- [API](#api)
- [Ветки и процесс разработки](#ветки-и-процесс-разработки)
- [Команда](#команда)

---

## Быстрый старт

**Требования:** Linux / WSL Ubuntu 20.04+, GCC 11+, CMake 3.16+

```bash
# 1. Установить зависимости
sudo apt install build-essential cmake libcurl4-openssl-dev

# 2. Клонировать и собрать
git clone https://github.com/essentomori/WebAgent-LAB.git
cd WebAgent-LAB
mkdir build && cd build
cmake ..
make

# 3. Запустить
./WebAgent
```

Ожидаемый вывод:

```
2026-03-30 23:44:49 [INFO] === WebAgent запускается ===
2026-03-30 23:44:49 [INFO] Используем hardcoded access_code=e87ccd-...
2026-03-30 23:44:49 [INFO] Цикл опроса запущен. Интервал: 60 сек.
2026-03-30 23:44:49 [INFO] Нет заданий (WAIT). Следующий опрос через 60 сек.
```

---

## Архитектура

```
┌─────────────────────────────────────────────────────────────┐
│                  Сервер  xdev.arkcom.ru:9999                 │
│                                                             │
│   /api/wa_reg/      /api/wa_task/      /api/wa_result/      │
└────────┬───────────────────┬──────────────────┬────────────┘
         │ HTTPS             │ HTTPS            │ HTTPS
         │                   │                  │
┌────────▼───────────────────▼──────────────────▼────────────┐
│                        WebAgent  (C++17)                    │
│                                                             │
│  main.cpp              polling.cpp         result_sender    │
│  ─────────────         ──────────────      ────────────     │
│  · Logger              · request_task()    · send_result()  │
│  · Http (libcurl)      · execute_task()    · multipart      │
│  · Json                · handle_conf()     · file attach    │
│  · register_agent()    · handle_file()                      │
│  · main()              · handle_task()                      │
│                        · handle_timeout()                   │
│                                                             │
│              agent.h  ─  общие типы и объявления            │
│              config.json  ─  uid, interval                  │
└─────────────────────────────────────────────────────────────┘
```

**Поток выполнения:**

```
Запуск
  └─ Есть HARDCODED_ACCESS_CODE?
       ├─ Да  → использовать напрямую
       └─ Нет → POST /wa_reg/ → сохранить access_code
                    │
                    └─ polling_loop() ─────────────────────────┐
                              │                                 │
                         POST /wa_task/                         │
                              │                                 │
                    ┌─────────▼──────────┐                     │
                    │   code_responce    │                      │
                    ├────────────────────┤                      │
                    │ 1  → execute_task()│                      │
                    │ 0  → sleep(N сек.) ├─────────────────────►┘
                    │ -2 → re-register   │
                    └────────────────────┘
                              │
                    execute_task() → send_result()
                                      └─ POST /wa_result/ multipart
```

---

## Структура проекта

```
WebAgent-LAB/
├── assets/
│   └── config.json          # uid агента и интервал опроса
├── include/
│   └── agent.h              # Config, AgentState, Task, Logger, Http, Json
├── src/
│   ├── main.cpp             # точка входа, регистрация, утилиты
│   ├── polling.cpp          # цикл опроса, все обработчики заданий
│   └── result_sender.cpp    # отправка результата с файлами
├── CMakeLists.txt
├── .gitignore
└── README.md
```

`config.json` копируется в `build/` автоматически при сборке через CMake.

---

## Конфигурация

**`assets/config.json`** — читается при старте:

```json
{
    "uid": "007",
    "description": "web-agent",
    "poll_interval_sec": 60
}
```

**`include/agent.h`** — параметры компиляции:

```cpp
namespace Config {
    const std::string BASE_URL              = "https://xdev.arkcom.ru:9999/app/webagent1/api";
    const std::string AGENT_UID             = "007";
    const std::string AGENT_DESC            = "web-agent";
    const std::string HARDCODED_ACCESS_CODE = "";   // вставить если агент уже зарегистрирован
    inline int        POLL_INTERVAL_SEC     = 60;
    const int         MAX_REG_RETRIES       = 3;
}
```

Если `HARDCODED_ACCESS_CODE` не пустой — агент пропускает регистрацию и сразу начинает опрос.

Интервал можно передать аргументом при запуске:

```bash
./WebAgent 30   # опрашивать каждые 30 секунд
```

---

## API

Базовый URL: `https://xdev.arkcom.ru:9999/app/webagent1/api`

---

### POST `/wa_reg/` — регистрация агента

Запрос:
```json
{
    "UID": "007",
    "descr": "web-agent"
}
```

Ответы:

| code_responce | Описание | Дополнительные поля |
|---|---|---|
| `0` | Успешная регистрация | `access_code` |
| `-3` | Агент уже зарегистрирован | `msg` |

---

### POST `/wa_task/` — запрос задания

Запрос:
```json
{
    "UID": "007",
    "descr": "web-agent",
    "access_code": "e87ccd-3146-0dcc-2aeb-796c4724"
}
```

Ответы:

| code_responce | Описание | Дополнительные поля |
|---|---|---|
| `1` | Задание есть | `task_code`, `session_id`, `options`, `status: RUN` |
| `0` | Заданий нет | `status: WAIT` |
| `-2` | Неверный код доступа | `msg` |

**Типы заданий (`task_code`):**

| task_code | Описание | Что писать в поле options |
|---|---|---|
| `CONF` | Изменить параметр в config.json | `{"key":"poll_interval_sec","value":"30"}` |
| `FILE` | Отправить файл серверу | `{"filename":"config.json"}` |
| `TASK` | Выполнить команду и вернуть вывод | `{"command":"uname -a"}` |
| `TIMEOUT` | Изменить интервал опроса | `{"interval":"30"}` |

---

### POST `/wa_result/` — отправка результата

Content-Type: `multipart/form-data`

| Поле | Тип | Описание |
|---|---|---|
| `result_code` | int | `0` — успех, `< 0` — ошибка |
| `result` | string (JSON) | `{"UID","access_code","message","files","session_id"}` |
| `file1` | file | Файл 1 (опционально) |
| `file2` | file | Файл 2 (опционально) |
| `file3...` | file | Далее по порядку |

Ответы:

| code_responce | Описание |
|---|---|
| `0` | Результат принят |
| `-3` | Ошибка загрузки файлов |

---

## Ветки и процесс разработки

### Структура веток

```
main
 └── dev
       ├── Kovalev       # essentomori  — lead + main.cpp, agent.h
       ├── Gomonov       # ItQ0n        — main.cpp, agent.h
       ├── Smirnov       # t9tu0        — polling.cpp
       ├── Pugovkin      # miroslav_pug — result_sender.cpp
       └── Naumov        # XXI_Primarch — docs, tests
```

### Правила

- **`main`** — только рабочий код. Прямые коммиты запрещены. Слияние через PR из `dev`, требует апрув от lead.
- **`dev`** — интеграционная. Сюда идут все PR с feature-веток. Должна собираться без ошибок.
- **Личные ветки** — каждый работает в своей ветке и открывает PR в `dev`.

### Процесс работы

```bash
# Начало задачи — обновить ветку от dev
git checkout Smirnov
git pull origin dev

# Работа, коммиты
git add src/polling.cpp
git commit -m "feat: добавлен обработчик TASK"
git push origin Smirnov

# Открыть PR: Smirnov → dev на GitHub
```

### Рекомендации по организации для команды 7 человек

**Lead (@essentomori)** — владеет `main`. Только он делает финальный merge из `dev` в `main`. Ревьюит все PR перед слиянием в `dev`.

**Архитектор (@KrlKot)** — не ведёт постоянную ветку. Создаёт ветки вида `arch/description` для архитектурных изменений (правки `agent.h`, структуры модулей). PR такой ветки требует отдельного обсуждения перед слиянием.

**Разработчики** (@ItQ0n, @t9tu0, @miroslav_pug) — работают в своих ветках. Один разработчик = одна ветка = один модуль. Коммиты небольшие и атомарные. Перед PR — `git pull origin dev` чтобы не было конфликтов.

**Тестировщик (@XXI_Primarch)** — тестирует на ветке `dev`. Если находит баг — открывает Issue на GitHub, назначает на ответственного разработчика. Не вносит правки в чужие модули.

**Техпис (@XXI_Primarch)** — ведёт ветку `Naumov`. Документация обновляется синхронно с кодом: если разработчик добавил новый `task_code` — техпис сразу обновляет README и API.md в том же спринте.

---

## Команда

| Участник | Ветка | Роль | Зона ответственности |
|---|---|---|---|
| @essentomori | `Kovalev` | Team Lead | Архитектура, ревью, `main` |
| @ItQ0n | `Gomonov` | Разработчик | `main.cpp`, `agent.h` |
| @t9tu0 | `Smirnov` | Разработчик | `polling.cpp` |
| @miroslav_pug | `Pugovkin` | Разработчик | `result_sender.cpp` |
| @KrlKot | `arch/*` | Архитектор | Проектирование модулей |
| @XXI_Primarch | `Naumov` | Техпис + Тестировщик | Документация, тесты |
