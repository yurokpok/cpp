# AI Code Analyzer

## Идея
AI Code Analyzer — это интеллектуальный инструмент для статического анализа кода на C++ и Python. Используя современные языковые модели (локальные через llama.cpp или удаленные API), он обнаруживает ошибки, потенциальные уязвимости и предоставляет рекомендации по улучшению кода.

## Возможности
- Анализ файлов — автоматическое сканирование .cpp и .py файлов
- Анализ строк кода — проверка кода прямо из командной строки
- Два источника ИИ — локальная модель или удаленный API
- Сохранение истории — хранение результатов анализа в SQLite БД
- Интерактивный режим — удобный диалоговый интерфейс с подсказками

## Использование

```bash
./ai_agent help
# или
./ai_agent --help
# или
./ai_agent -h

Анализ файлов

```bash
# Автоопределение языка
./ai_agent analyze путь/к/файлу.cpp
./ai_agent analyze путь/к/скрипту.py

# С указанием языка
./ai_agent analyze файл.txt cpp       # обработать как C++
./ai_agent analyze файл.txt python    # обработать как Python

Анализ кода из строки

```bash
# Однострочный Python код
./ai_agent code "def test(): return 1" python

# Многострочный C++ код
./ai_agent code $'#include <iostream>\nint main() { std::cout << "Hello"; }' cpp

Интерактивный режим

```bash
./ai_agent interactive
=== AI Code Analyzer - Интерактивный режим ===
> /help          # Показать все команды
> /context       # Вкл/выкл сохранение ответов
> /saved         # Показать сохраненные ответы
> /clear         # Очистить сохраненные ответы
> /lang python   # Установить язык анализа
> /file test.cpp # Проанализировать файл
> /local         # Переключиться на локальную модель
> /remote        # Переключиться на удаленный API
> /model-info    # Показать информацию о модели
> /quit          # Выйти

Агент поддерживает сохранение контекста разговора в SQLite базе данных:

```bash
# В интерактивном режиме
> /context  # Включить сохранение
> /context  # Выключить сохранение
> /saved
> /clear

Переключение между моделями ИИ

```bash
# Переключение на локальную модель
> /local
✓ Переключено на ЛОКАЛЬНУЮ МОДЕЛЬ
Сервер: 127.0.0.1:8080

# Переключение на удаленный API
> /remote
✓ Переключено на УДАЛЕННЫЙ API
Сервер: ai-api.hurated.com:443


## Установка и сборка

```bash
git clone <ваш-репозиторий>
cd ai_agent_example
mkdir build && cd build
cmake ..
make -j

## Терминал 1: Запуск сервера llama.cpp

```bash
git clone https://github.com/ggerganov/llama.cpp
cd llama.cpp
cmake -B build
cmake --build build --config Release -- -j$(sysctl -n hw.ncpu)

Добавить models/qwen2.5-0.5b-instruct-q4_k_m.gguf 

```bash
./llama.cpp/build/bin/llama-server \
  -m models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  -c 4096 -ngl 999 \
  --host 127.0.0.1 --port 8080
  
## Терминал 2: Проверка работы сервера

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions   -H "Content-Type: application/json"   -d '{
    "model": "local-gguf",
    "messages": [
      {"role":"system","content":"You are a helpful coding assistant."},
      {"role":"user","content":"Поясни в двух предложениях, что такое цикл в C++."}
    ],
    "max_tokens": 200,
    "temperature": 0.7,
    "top_p": 0.9
  }' | jq -r '.choices[0].message.content'

