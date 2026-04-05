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

flowchart TB
    subgraph Server["Сервер xdev.arkcom.ru:9999"]
        direction LR
        API1["/api/wa_reg/"]
        API2["/api/wa_task/"]
        API3["/api/wa_result/"]
    end

    subgraph Agent["WebAgent (C++17)"]
        direction TB
        subgraph Components[" "]
            direction LR
            Main["main.cpp<br/><small>• Logger<br/>• Http (libcurl)<br/>• Json<br/>• register_agent()<br/>• main()</small>"]
            Polling["polling.cpp<br/><small>• request_task()<br/>• execute_task()<br/>• handle_conf()<br/>• handle_file()<br/>• handle_task()<br/>• handle_timeout()</small>"]
            Sender["result_sender<br/><small>• send_result()<br/>• multipart<br/>• file attach</small>"]
        end
        Other["agent.h — общие типы и объявления<br/>config.json — uid, interval"]
    end

    Server -- "HTTPS" --> Agent
    Server -- "HTTPS" --> Agent
    Server -- "HTTPS" --> Agent

**Поток выполнения:**

flowchart TD
    Start([🚀 Запуск])

    subgraph Init[Инициализация]
        Check{Есть HARDCODED_ACCESS_CODE?}
        Use[использовать напрямую]
        Register[📝 POST /wa_reg/]
        Save[сохранить access_code]
    end

    subgraph MainLoop[Основной цикл - polling_loop]
        Task[📤 POST /wa_task/]
        Response{code_response}
        Sleep[💤 sleep N сек.]
        ReReg[🔄 re-register]
    end

    subgraph Execute[Выполнение задачи]
        ExecTask[execute_task]
        SendResult[send_result]
        PostResult[📎 POST /wa_result/ multipart]
    end

    Start --> Check

    Check -->|Да| Use
    Use --> MainLoop

    Check -->|Нет| Register
    Register --> Save
    Save --> MainLoop

    MainLoop --> Task
    Task --> Response

    Response -->|1| ExecTask
    ExecTask --> SendResult
    SendResult --> PostResult
    PostResult --> Task

    Response -->|0| Sleep
    Sleep --> Task

    Response -->|-2| ReReg
    ReReg --> Register

---

## Структура проекта

flowchart LR
    subgraph Repo["📦 WebAgent-LAB/"]
        direction TB

        subgraph Assets["📁 assets/"]
            Config["config.json<br/><small>uid агента и интервал опроса</small>"]
        end

        subgraph Include["📁 include/"]
            AgentH["agent.h<br/><small>Config, AgentState, Task,<br/>Logger, Http, Json</small>"]
        end

        subgraph Src["📁 src/"]
            Main["main.cpp<br/><small>точка входа, регистрация, утилиты</small>"]
            Polling["polling.cpp<br/><small>цикл опроса, обработчики заданий</small>"]
            ResultSender["result_sender.cpp<br/><small>отправка результата с файлами</small>"]
        end

        CMake["⚙️ CMakeLists.txt"]
        GitIgnore["🙈 .gitignore"]
        Readme["📖 README.md"]
    end

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

flowchart LR
    Main["main"] --> Dev["dev"]

    Dev --> A["👑 Kovalev<br/>@essentomori<br/>lead + main.cpp, agent.h"]
    Dev --> B["💻 Gomonov<br/>@ItQ0n<br/>main.cpp, agent.h"]
    Dev --> C["⚙️ Smirnov<br/>@t9tu0<br/>polling.cpp"]
    Dev --> D["📤 Pugovkin<br/>@miroslav_pug<br/>result_sender.cpp"]
    Dev --> E["📚 Naumov<br/>@XXI_Primarch<br/>docs, tests"]

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
