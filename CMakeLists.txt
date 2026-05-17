cmake_minimum_required(VERSION 3.10)
project(WebAgent VERSION 1.0.0 LANGUAGES CXX)

# Стандарт C++17 (нужен для std::optional и инициализаторов в if/for)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Поиск зависимостей (CURL)
find_package(CURL REQUIRED)

# 2. Указываем, где лежат наши заголовочные файлы
include_directories(include)

# 3. Список исходных файлов
set(SOURCES
    src/main.cpp
    src/polling.cpp
    src/result_sender.cpp
)

# 4. Создаем исполняемый файл
add_executable(${PROJECT_NAME} ${SOURCES})

# 5. Линковка библиотек (подключаем CURL к нашему агенту)
target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)

# 6. Настройки для Windows
if(WIN32)
    # Минимальная версия Windows API — Windows 7 (0x0601)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _WIN32_WINNT=0x0601)
    # Отключаем предупреждения MSVC об устаревших POSIX-именах (localtime и др.)
    if(MSVC)
        target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    # Дополнительные библиотеки Windows, необходимые libcurl
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 wldap32 crypt32)
endif()

# 6. (Опционально) Копируем config.json в папку со сборкой, чтобы он был под рукой
configure_file(assets/config.json ${CMAKE_BINARY_DIR}/config.json COPYONLY)

# Вывод информации для удобства
message(STATUS "CURL found: ${CURL_LIBRARIES}")
