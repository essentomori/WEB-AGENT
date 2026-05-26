# WebAgent — Техническая документация

В этом документе описаны внутренняя архитектура, все публичные символы и точный контракт API между агентом и сервером.

---

## Содержание

- [Архитектура](#архитектура)
- [Последовательность запуска](#последовательность-запуска)
- [Конечный автомат цикла опроса](#конечный-автомат-цикла-опроса)
- [namespace Config](#namespace-config)
- [namespace Logger](#namespace-logger)
- [namespace Http](#namespace-http)
- [namespace Json](#namespace-json)
- [struct AgentState](#struct-agentstate)
- [struct Task](#struct-task)
- [class SafeQueue\<T\>](#class-safequeuet)
- [class ThreadPool](#class-threadpool)
- [Обработчики задач](#обработчики-задач)
- [Контракт серверного API](#контракт-серверного-api)
  - [POST /wa_reg/](#post-wa_reg)
  - [POST /wa_task/](#post-wa_task)
  - [POST /wa_result/](#post-wa_result)
- [Справочник кодов ошибок](#справочник-кодов-ошибок)
- [Алгоритм адаптивного backoff](#алгоритм-адаптивного-backoff)

---

## Архитектура

```
main.cpp                  polling.cpp                  result_sender.cpp
──────────                ───────────────────────      ─────────────────
signal_handler()          Config::load()               send_result()
main()                    Logger::init / log()            └─ curl_mime (multipart)
  │                       register_agent()
  │                       try_reregister()
  └─► polling_loop() ──►  request_task()
          │               dispatch_task()
          │                 └─► карта HANDLERS
          │                       ├─ handle_conf()
          │                       ├─ handle_file()
          │                       ├─ handle_task()
          │                       └─ handle_timeout()
          └─► ThreadPool::submit(fn)
                └─► SafeQueue::push()
                      └─► воркер-потоки: pop() и выполнение fn
```

**Модель потоков:**
- 1 поток опроса — выполняет `polling_loop()`, никогда не блокируется на выполнении задачи
- N воркер-потоков — создаются в `ThreadPool`, каждый одновременно обрабатывает одну `dispatch_task()`
- Всё логирование сериализуется через единственный `std::mutex`

---

## Последовательность запуска

```
main()
 1. Установка SIGINT / SIGTERM → signal_handler()
 2. Logger::init(INFO)
 3. Config::load(path)           ← бросает исключение при отсутствии base_url или uid
 4. Config::load_access_code()   ← читает "access_code" из config.json
      ├── найден  → g_state.access_code = code
      └── отсутствует → register_agent()
                          ├── успех → g_state.access_code = new_code
                          └── ошибка → exit(1)
 5. polling_loop()               ← блокирует до shutdown_requested
 6. exit(0)
```

---

## Конечный автомат цикла опроса

```
              ┌─────────────────────────┐
              │          IDLE           │◄──────────────────────────┐
              │  (ожидание poll_interval)│                          │
              └────────────┬────────────┘                          │
                           │ пробуждение                           │
                           ▼                                       │
                    request_task()                                  │
                    ┌──────┼──────┐                                │
                    │      │      │                                │
                  rc=1    rc=0  rc=-99                            │
                 (задача) (ждём) (ошибка)                         │
                    │      │      │                                │
                    │      │      ▼                                │
                    │      │   BACKOFF                             │
                    │      │   интервал *= 2 (≤ backoff_max_sec)  │
                    │      │      │                                │
                    │      └──────┴─────────────────────────────►─┘
                    │
                    ▼
             ThreadPool::submit(task)   ← неблокирующий вызов
                    │
                    ▼
             воркер: dispatch_task(task)
                    │
                    └─► handler() → send_result()
```

`rc=-2` (недействительный access_code) запускает `try_reregister()` перед продолжением работы.

---

## namespace Config

**Объявлено в:** `include/agent.h` (объявления), `src/polling.cpp` (реализации)

Все переменные — `inline`-глобальные: доступны для чтения и записи из любой единицы трансляции.

### Переменные

| Переменная | Тип | По умолчанию | Описание |
|---|---|---|---|
| `CONFIG_FILE` | `std::string` | `""` | Абсолютный или относительный путь к файлу конфига. Устанавливается в `load()`. |
| `BASE_URL` | `std::string` | `""` | Корневой URL для всех вызовов API. Берётся из JSON-поля `base_url` или env `AGENT_BASE_URL`. |
| `AGENT_UID` | `std::string` | `""` | Идентификатор агента. Берётся из JSON-поля `uid` или env `AGENT_UID`. |
| `AGENT_DESC` | `std::string` | `"web-agent"` | Метка, отправляемая в теле каждого запроса. |
| `POLL_INTERVAL_SEC` | `int` | `60` | Нормальный интервал опроса. Может изменяться во время работы (задачи TIMEOUT / CONF). |
| `BACKOFF_MAX_SEC` | `int` | `300` | Максимальный интервал опроса в режиме backoff. |
| `MAX_REG_RETRIES` | `int` | `3` | Максимальное число попыток регистрации до прерывания. |
| `REQUEST_TIMEOUT_SEC` | `long` | `30` | libcurl `CURLOPT_TIMEOUT`. |
| `CONNECT_TIMEOUT_SEC` | `long` | `10` | libcurl `CURLOPT_CONNECTTIMEOUT`. |
| `TASK_DIR` | `std::string` | `"tasks"` | Зарезервированный каталог для локального хранения задач. |
| `RESULT_DIR` | `std::string` | `"results"` | Зарезервированный каталог для локального кэша результатов. |
| `LOG_DIR` | `std::string` | `"logs"` | Каталог для лог-файлов. |

### Функции

---

#### `void Config::load(const std::string& path)`

Загружает конфигурацию из JSON-файла, затем применяет переопределения из переменных окружения.

**Параметры**
- `path` — путь к `config.json`

**Бросает**
- `std::runtime_error` — если файл отсутствует или пуст
- `std::runtime_error` — если после загрузки отсутствуют `base_url` или `uid`
- `std::runtime_error` — если `poll_interval_sec` ≤ 0

**Применяемые переопределения из env (наивысший приоритет):**
```
AGENT_BASE_URL       → Config::BASE_URL
AGENT_UID            → Config::AGENT_UID
AGENT_POLL_INTERVAL  → Config::POLL_INTERVAL_SEC
```

**Пример `config.json`:**
```json
{
    "base_url": "https://xdev.arkcom.ru:9999/app/webagent1/",
    "uid": "agent-01",
    "poll_interval_sec": 30
}
```

---

#### `bool Config::save_access_code(const std::string& code)`

Читает `CONFIG_FILE`, обновляет или добавляет поле `"access_code"` и записывает результат обратно.
Если файл не существует, создаёт минимальный JSON-объект с полями `uid` и `base_url`.

**Возвращает** `true` при успехе, `false` если файл не удаётся записать.

---

#### `bool Config::load_access_code(std::string& out)`

Читает `CONFIG_FILE` и извлекает поле `"access_code"` в `out`.

**Возвращает** `true` и заполняет `out`, если поле существует и непусто; иначе `false`.

---

## namespace Logger

**Объявлено в:** `include/agent.h` (объявления + inline-обёртки), `src/polling.cpp` (реализация)

Потокобезопасен. Весь вывод сериализуется через единственный `std::mutex`.

### Формат лога

```
2024-05-18 14:32:01.247 [INFO   ] [T:3a1f] [task=TASK] [session=39493313738556796:157] сообщение
```

Поля `[task=…]` и `[session=…]` опускаются, если соответствующие параметры — пустые строки.

### Уровни

| Enum | Строка | Числовое значение |
|---|---|---|
| `Level::DEBUG` | `DEBUG  ` | 0 |
| `Level::INFO` | `INFO   ` | 1 |
| `Level::WARNING` | `WARNING` | 2 |
| `Level::ERR` | `ERROR  ` | 3 |
| `Level::CRIT` | `CRIT   ` | 4 |

Сообщения ниже настроенного минимального уровня отбрасываются без захвата мьютекса.

### Функции

---

#### `void Logger::init(Level min_level, bool to_file, const std::string& log_file)`

Инициализирует логгер. Должна быть вызвана один раз до первого лог-сообщения.

**Параметры**
- `min_level` — сообщения ниже этого уровня молча отбрасываются (по умолчанию: `Level::INFO`)
- `to_file` — если `true`, вывод дублируется в `log_file` (по умолчанию: `false`)
- `log_file` — путь к лог-файлу, открываемому в режиме дозаписи (по умолчанию: `""`)

Если `to_file` равно `true`, но файл не удаётся открыть, запись в файл молча отключается, а вывод в stdout продолжается в штатном режиме.

---

#### `void Logger::log(Level level, const std::string& msg, const std::string& task_id, const std::string& session_id)`

Основная функция логирования. Формирует структурированную строку и атомарно записывает её в stdout (и опционально в лог-файл).

**Параметры**
- `level` — уровень серьёзности
- `msg` — тело сообщения
- `task_id` — опциональный код задачи (например `"CONF"`, `"TASK"`). Не выводится, если пустой.
- `session_id` — опциональный идентификатор сессии. Не выводится, если пустой.

---

#### Удобные обёртки

Все обёртки вызывают `Logger::log` с соответствующим уровнем. `task_id` и `session_id` по умолчанию равны `""`.

```cpp
void Logger::debug  (const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
void Logger::info   (const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
void Logger::warn   (const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
void Logger::err    (const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
void Logger::crit   (const std::string& msg, const std::string& task_id = "", const std::string& session_id = "");
```

---

## namespace Http

**Объявлено в:** `include/agent.h` (объявление), `src/main.cpp` (реализация)

### struct Http::Response

```cpp
struct Response {
    int         status_code = 0;   // HTTP-статус (например 200, 404)
    std::string body;              // сырое тело ответа
};
```

### Функции

---

#### `Http::Response Http::post(const std::string& url, const std::string& body_json)`

Отправляет синхронный `POST`-запрос с `Content-Type: application/json`.

**Параметры**
- `url` — полный URL
- `body_json` — сериализованная JSON-строка

**Возвращает** `Http::Response` с полями `status_code` и `body`.

**Бросает** `std::runtime_error`, если `curl_easy_init()` завершился с ошибкой или передача не удалась на транспортном уровне (например, ошибка разрешения DNS, отказ в подключении).

**Таймауты:** используются `Config::REQUEST_TIMEOUT_SEC` и `Config::CONNECT_TIMEOUT_SEC`.

> **Примечание:** верификация TLS-сертификата в данный момент отключена (`CURLOPT_SSL_VERIFYPEER = 0`, `CURLOPT_SSL_VERIFYHOST = 0`). Для production-окружения укажите CA bundle и включите верификацию.

---

## namespace Json

**Объявлено в:** `include/agent.h` (объявление), `src/main.cpp` (реализация)

Минимальные JSON-утилиты. Не полноценный парсер — обрабатывает плоские структуры ключ-значение, используемые в серверном API.

### Функции

---

#### `std::optional<std::string> Json::get(const std::string& json, const std::string& key)`

Извлекает строковое или примитивное значение верхнего уровня из JSON-объекта по ключу.

**Параметры**
- `json` — сырая JSON-строка
- `key` — имя поля для поиска

**Возвращает** значение как `std::string`, если найдено, или `std::nullopt`, если ключ отсутствует либо JSON некорректен в этом месте.

Обрабатывает как строковые значения в кавычках (`"value"`), так и непроцитированные примитивы (числа, булевы значения).

---

#### `std::string Json::build(std::initializer_list<std::pair<std::string, std::string>> pairs)`

Строит плоский JSON-объект из списка строковых пар ключ-значение. Все значения выводятся как JSON-строки (в кавычках).

**Пример:**
```cpp
std::string body = Json::build({
    {"UID",         "agent-01"},
    {"descr",       "web-agent"},
    {"access_code", "abc-123"}
});
// → {"UID":"agent-01","descr":"web-agent","access_code":"abc-123"}
```

> **Важно:** не используйте эту функцию для целых чисел или булевых значений — они будут сериализованы как строки. Сервер принимает такой формат для всех полей, используемых агентом.

---

## struct AgentState

**Объявлено в:** `include/agent.h`
**Глобальный экземпляр:** `AgentState g_state` (в `src/main.cpp`)

Хранит runtime-идентичность агента и флаг завершения.

```cpp
struct AgentState {
    std::string        access_code;           // токен, полученный после регистрации
    std::atomic<bool>  shutdown_requested;    // устанавливается signal_handler(); проверяется каждую секунду
    bool is_registered() const;               // true, если access_code непустой
    void reset();                             // очищает access_code (перед повторной регистрацией)
};
```

| Поле | Потокобезопасность | Описание |
|---|---|---|
| `access_code` | Чтение/запись только из потока опроса | Учётные данные, отправляемые в каждом запросе к `/wa_task/` и `/wa_result/`. |
| `shutdown_requested` | `std::atomic<bool>` — безопасно из обработчика сигнала | Проверяется на каждой итерации опроса и внутри цикла ожидания. |
| `is_registered()` | Только чтение, поток опроса | Возвращает `!access_code.empty()`. |
| `reset()` | Поток опроса | Очищает `access_code` перед попыткой повторной регистрации. |

---

## struct Task

**Объявлено в:** `include/agent.h`

Представляет одну задачу, полученную от сервера.

```cpp
struct Task {
    std::string task_code;    // ключ обработчика: "CONF", "FILE", "TASK", "TIMEOUT"
    std::string session_id;   // непрозрачный идентификатор, возвращаемый с результатом
    std::string options;      // JSON-экранированная строка опций от сервера
    std::string status;       // серверный статус задачи (информационный)
    bool is_valid() const;    // true, если task_code и session_id непустые
};
```

`options` приходит от сервера как JSON-экранированная строка (например `{\"key\":\"uid\",\"value\":\"007\"}`). Каждый обработчик внутри вызывает `clean_options()` для снятия экранирования и извлечения чистого JSON-объекта перед чтением отдельных полей.

`is_valid()` проверяется в `request_task()` перед отправкой задачи в пул потоков. Невалидные payload'ы молча отбрасываются (обрабатываются как `code_response: 0`).

---

## class SafeQueue\<T\>

**Объявлено в:** `include/thread_pool.h` — header-only шаблон

Ограниченная блокирующая потокобезопасная FIFO-очередь на основе `std::queue<T>`.

```cpp
template<typename T>
class SafeQueue {
public:
    void        push(T item);       // добавить в очередь; будит один заблокированный pop()
    bool        pop(T& out);        // блокирует до появления элемента или вызова stop()
    void        stop();             // разблокирует все pop() навсегда
    std::size_t size() const;       // приблизительная глубина очереди (захватывает мьютекс)
};
```

### Описание методов

---

#### `void SafeQueue<T>::push(T item)`

Перемещает `item` в очередь и вызывает `notify_one()` на внутренней условной переменной.

**Потокобезопасность:** безопасно вызывать из любых потоков одновременно.

---

#### `bool SafeQueue<T>::pop(T& out)`

Блокирует вызывающий поток до тех пор, пока не появится элемент или не будет вызван `stop()`.

**Возвращает**
- `true` — `out` содержит извлечённый элемент
- `false` — был вызван `stop()` и очередь пуста; вызывающий код должен выйти из цикла

---

#### `void SafeQueue<T>::stop()`

Устанавливает внутренний флаг `atomic<bool>` и вызывает `notify_all()`. Все текущие и будущие вызовы `pop()` вычерпают оставшиеся элементы, а затем вернут `false`. Идемпотентен.

---

## class ThreadPool

**Объявлено в:** `include/thread_pool.h` — header-only

Пул воркер-потоков фиксированного размера. Задачи передаются как `std::function<void()>` и выполняются в порядке FIFO.

```cpp
class ThreadPool {
public:
    explicit ThreadPool(std::size_t n_workers);
    ~ThreadPool();                               // вызывает shutdown()

    void        submit(std::function<void()> fn); // добавить задачу в очередь; неблокирующий
    void        shutdown();                        // вычерпать очередь, дождаться всех потоков
    std::size_t pending() const;                   // текущая глубина очереди
};
```

### Описание методов

---

#### `ThreadPool::ThreadPool(std::size_t n_workers)`

Немедленно порождает `n_workers` потоков. Каждый поток зациклен на `SafeQueue::pop()`.

**Параметры**
- `n_workers` — количество воркер-потоков. Управляется во время работы через переменную окружения `AGENT_WORKERS` (по умолчанию: `4`).

---

#### `void ThreadPool::submit(std::function<void()> fn)`

Помещает `fn` в очередь. Возвращается немедленно, не ожидая выполнения.

**Бросает** `std::runtime_error`, если `shutdown()` уже был вызван.

---

#### `void ThreadPool::shutdown()`

Сигнализирует очереди об остановке, ожидает завершения текущих задач во всех воркер-потоках, затем выполняет join и очищает их. Идемпотентен — безопасно вызывать многократно.

Вызывается автоматически деструктором и явно функцией `polling_loop()` перед возвратом.

---

#### `std::size_t ThreadPool::pending() const`

Возвращает количество задач, находящихся в данный момент в очереди (не считая выполняемую). Используется только для логирования.

---

## Обработчики задач

Все обработчики — `static`-функции в `src/polling.cpp`. Они регистрируются в карте `HANDLERS` и вызываются функцией `dispatch_task()` внутри воркер-потока.

**Общий контракт:**
- Получает `const Task&`
- Парсит `task.options` с помощью `clean_options()` и `pick()`
- Вызывает `send_result(task.session_id, result_code, message, files)` ровно один раз
- Логирует ход выполнения и ошибки с контекстом `[task=task_code, session=session_id]`

---

### `handle_conf`

Изменяет пару ключ-значение в `Config::CONFIG_FILE`.

**Поля options:**
| Поле | Обязательно | Описание |
|---|---|---|
| `key` | Да | JSON-ключ для обновления |
| `value` | Да | Новое значение (строка или числовая строка) |
| `val` | Альтернатива | Форма массива `["значение"]` — используется первый элемент |

**Побочный эффект:** если `key == "poll_interval_sec"`, `Config::POLL_INTERVAL_SEC` обновляется в памяти немедленно.

**Коды результата:**
| Код | Значение |
|---|---|
| `0` | Конфиг успешно обновлён |
| `-1` | Некорректная структура options или ошибка записи файла |

---

### `handle_file`

Отправляет локальный файл на сервер как multipart-вложение.

**Поля options:**
| Поле | Обязательно | Описание |
|---|---|---|
| `filename` | Нет | Путь к файлу. По умолчанию используется `Config::CONFIG_FILE`, если поле отсутствует или равно `"config.json"`. |

**Коды результата:**
| Код | Значение |
|---|---|
| `0` | Файл прикреплён и отправлен |
| `-1` | Файл не найден по указанному пути |

---

### `handle_task`

Выполняет команду оболочки через `popen()` и возвращает stdout.

**Поля options:**
| Поле | Обязательно | Описание |
|---|---|---|
| `command` | Да | Строка команды оболочки |

Вывод усекается до 2000 символов. Пустой вывод возвращается как `"(no output)"`.

**Коды результата:**
| Код | Значение |
|---|---|
| `0` | Команда выполнена (независимо от кода завершения) |
| `-1` | Поле `command` отсутствует |

> **Безопасность:** агент выполняет команды с теми же привилегиями, что и его процесс. Убедитесь, что серверный бэкенд надёжен, а API-эндпоинт защищён аутентификацией.

---

### `handle_timeout`

Обновляет интервал опроса в памяти. На диск **не записывает** (для сохранения используйте `CONF`).

**Поля options:**
| Поле | Обязательно | Описание |
|---|---|---|
| `interval` или `INTERVAL` | Да | Новый интервал в секундах (целое число или строка с числом) |

**Коды результата:**
| Код | Значение |
|---|---|
| `0` | Интервал обновлён |
| `-1` | Поле отсутствует или значение ≤ 0 |

---

## Контракт серверного API

Все эндпоинты находятся под `Config::BASE_URL`. Тела запросов — `application/json`. В поле `code_responce` сервера содержится опечатка, унаследованная от бэкенда — агент воспроизводит её точно.

---

### POST /wa_reg/

Регистрирует агента и получает `access_code`.

**Тело запроса:**
```json
{
    "UID":   "agent-01",
    "descr": "web-agent"
}
```

**Ответ:**

| `code_responce` | Значение | Дополнительные поля |
|---|---|---|
| `0` | Регистрация успешна | `"access_code": "uuid-string"` |
| `-3` | Уже зарегистрирован | — (агент загружает сохранённый код из файла) |
| другое | Регистрация отклонена | — |

**Обработка ошибок:**
- HTTP ≠ 200 → залогировать ошибку, вернуть `false`
- `code_responce` отсутствует → залогировать ошибку парсинга, вернуть `false`
- `code_responce == 0`, но `access_code` отсутствует → залогировать ошибку, вернуть `false`
- `code_responce == -3` → попытаться загрузить `access_code` из `config.json`

---

### POST /wa_task/

Опрашивает наличие ожидающей задачи.

**Тело запроса:**
```json
{
    "UID":         "agent-01",
    "descr":       "web-agent",
    "access_code": "uuid-string"
}
```

**Ответ:**

| `code_responce` | Значение | Дополнительные поля |
|---|---|---|
| `1` | Задача доступна | `task_code`, `session_id`, `options`, `status` |
| `0` | Задач нет (ждать) | — |
| `-2` | Недействительный `access_code` | — (запускает повторную регистрацию) |
| другое | Неожиданный ответ | — (логируется как предупреждение, обрабатывается как сетевая ошибка) |

**Пример payload задачи:**
```json
{
    "code_responce": 1,
    "task_code":     "TASK",
    "session_id":    "39493313738556796:157",
    "options":       "{\"command\":\"uname -a\"}",
    "status":        "RUN"
}
```

---

### POST /wa_result/

Отправляет результат задачи. Использует `multipart/form-data` — **не** `application/json`.

**Поля формы:**

| Имя поля | Тип | Описание |
|---|---|---|
| `result_code` | string | Целочисленный код результата: `"0"` = успех, `"-1"` = ошибка |
| `result` | string | JSON-строка (см. ниже) |
| `file1`, `file2`, … | file | Опциональные файловые вложения |

**JSON поля `result`:**
```json
{
    "UID":         "agent-01",
    "descr":       "web-agent",
    "access_code": "uuid-string",
    "message":     "Linux hostname 5.15.0 ...",
    "files":       "0",
    "session_id":  "39493313738556796:157"
}
```

**Ответ:**

| `code_responce` | Значение |
|---|---|
| `0` | Результат принят |
| другое | Серверная ошибка (логируется, `send_result()` возвращает `false`) |

---

## Справочник кодов ошибок

### Внутренние коды возврата из `request_task()`

| Код | Значение |
|---|---|
| `1` | Задача получена и валидна |
| `0` | Задач нет (нормальное ожидание) |
| `-2` | Сервер отклонил `access_code` |
| `-99` | Сетевая ошибка, HTTP-ошибка или некорректный ответ |

### Коды результата задачи (отправляются на сервер)

| Код | Назначение |
|---|---|
| `0` | Успех |
| `-1` | Ошибка уровня обработчика (некорректные options, файл не найден и т.д.) |

---

## Алгоритм адаптивного backoff

Когда `request_task()` возвращает `-99` (любой сбой связи), цикл опроса входит в режим backoff:

```
при первой ошибке:
    backoff_step = max(current_interval, 5)
    in_backoff   = true

при каждой последующей ошибке:
    current_interval = backoff_step
    backoff_step     = min(backoff_step * 2, BACKOFF_MAX_SEC)

при любом успешном ответе (rc == 0 или rc == 1):
    in_backoff       = false
    backoff_step     = POLL_INTERVAL_SEC
    current_interval = POLL_INTERVAL_SEC
```

**Пример прогрессии** при `poll_interval_sec = 60` и `backoff_max_sec = 300`:

| Попытка | `current_interval` |
|---|---|
| 1-я ошибка | 60 с |
| 2-я ошибка | 120 с |
| 3-я ошибка | 240 с |
| 4-я ошибка | 300 с (ограничение) |
| восстановление | 60 с (сброс) |

Ожидание реализовано как цикл с тиками по 1 секунде, чтобы `shutdown_requested` проверялся каждую секунду вне зависимости от текущего интервала.
