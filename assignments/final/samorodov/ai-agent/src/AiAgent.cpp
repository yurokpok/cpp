#include "AiAgent.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <regex>
#include <curl/curl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

using nlohmann::json;

// Callback для curl
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* response) {
    size_t total_size = size * nmemb;
    response->append((char*)contents, total_size);
    return total_size;
}

// Конструктор и деструктор
AiAgent::AiAgent() : db_(nullptr), context_enabled_(false) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        db_path_ = std::string(cwd) + "/ai_responses.db";
    }
}

AiAgent::~AiAgent() {
    closeDatabase();
}

// --------- utils IO ----------
bool AiAgent::readWholeFile(const std::string& path, std::string& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { 
        if (err) *err = "Cannot open file: " + path; 
        return false; 
    }
    std::ostringstream ss; 
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// --------- JSON loaders ----------
bool AiAgent::loadConfig(const std::string& path, std::string* err) {
    std::string s;
    if (!readWholeFile(path, s, err)) return false;
    
    try {
        auto j = json::parse(s);
        
        // Обязательные поля для удаленного API
        if (j.contains("host")) cfg_.host = j.at("host").get<std::string>();
        if (j.contains("port")) cfg_.port = j.at("port").get<std::string>();
        if (j.contains("api_key")) cfg_.api_key = j.at("api_key").get<std::string>();
        
        // Источник инференса
        if (j.contains("inference_source")) {
            cfg_.inference_source = j.at("inference_source").get<std::string>();
        }
        
        // Локальные настройки
        if (j.contains("local_model")) {
            auto local = j["local_model"];
            if (local.contains("host")) cfg_.local_host = local.at("host").get<std::string>();
            if (local.contains("port")) cfg_.local_port = local.at("port").get<int>();
            if (local.contains("model_path")) cfg_.local_model_path = local.at("model_path").get<std::string>();
        }
        
        return true;
    } catch (const std::exception& e) {
        if (err) *err = std::string("Config parse error: ") + e.what();
        return false;
    }
}

bool AiAgent::loadPrompt(const std::string& path, std::string* err) {
    std::string s;
    if (!readWholeFile(path, s, err)) return false;
    try {
        json j = json::parse(s);
        if (j.is_string()) {
            prompt_ = j.get<std::string>();
        } else if (j.is_object() && j.contains("prompt")) {
            prompt_ = j.at("prompt").get<std::string>();
        } else {
            if (err) *err = "Prompt JSON must be string or object with key 'prompt'";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = std::string("Prompt parse error: ") + e.what();
        return false;
    }
}

// Определение языка программирования
std::string AiAgent::detectLanguage(const std::string& code) const {
    std::string lower_code = code;
    std::transform(lower_code.begin(), lower_code.end(), lower_code.begin(), ::tolower);
    
    // Эвристики для C++
    if (lower_code.find("#include") != std::string::npos ||
        lower_code.find("using namespace") != std::string::npos ||
        lower_code.find("std::") != std::string::npos ||
        lower_code.find("int main()") != std::string::npos ||
        lower_code.find("cout") != std::string::npos ||
        lower_code.find("cin") != std::string::npos) {
        return "cpp";
    }
    
    // Эвристики для Python
    if (lower_code.find("def ") != std::string::npos ||
        lower_code.find("import ") != std::string::npos ||
        lower_code.find("from ") != std::string::npos ||
        lower_code.find("print(") != std::string::npos ||
        (lower_code.find("class ") != std::string::npos && 
         lower_code.find(":") != std::string::npos) ||
        lower_code.find("__init__") != std::string::npos) {
        return "python";
    }
    
    return "auto";
}

// Построение промпта для анализа
std::string AiAgent::buildAnalysisPrompt(const std::string& code, 
                                        const std::string& language,
                                        bool is_complete_code) const {
    std::ostringstream prompt;
    
    prompt << "Ты - опытный программист-аналитик. Проанализируй код и выдай результат в ЧЕТКОМ ФОРМАТЕ:\n\n";
    
    std::string lang = language;
    if (lang == "auto") {
        lang = detectLanguage(code);
    }
    
    prompt << "ЯЗЫК: " << (lang == "cpp" ? "C++" : "Python") << "\n\n";
    
    if (is_complete_code) {
        prompt << "ВНИМАНИЕ: Анализируй код как ЕДИНОЕ ЦЕЛОЕ, не комментируй каждую строку отдельно.\n\n";
    }
    
    prompt << "ФОРМАТ ОТВЕТА (СТРОГО СОБЛЮДАЙ):\n\n";
    prompt << "=== ОШИБКИ ===\n";
    prompt << "1. [Тип ошибки] [Строка]: Описание\n";
    prompt << "2. ...\n\n";
    
    prompt << "=== РЕКОМЕНДАЦИИ ===\n";
    prompt << "1. [Категория]: Рекомендация\n";
    prompt << "2. ...\n\n";
    
    prompt << "=== ОБЩАЯ ОЦЕНКА ===\n";
    prompt << "[Краткая оценка качества кода, 1-2 предложения]\n\n";
    
    prompt << "КОД ДЛЯ АНАЛИЗА:\n```" << lang << "\n" << code << "\n```\n\n";
    prompt << "ВАЖНО: Отвечай ТОЛЬКО в указанном формате, без лишних объяснений и без комментариев к каждой строке.";
    
    return prompt.str();
}

// -------- Низкоуровневый HTTPS POST на /api/generate --------
std::optional<std::string> AiAgent::httpsPostGenerate(const std::string& jsonBody, std::string* err) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { 
        if (err) *err = "SSL_CTX_new failed"; 
        return std::nullopt; 
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { 
        if (err) *err = "socket failed"; 
        SSL_CTX_free(ctx); 
        return std::nullopt; 
    }

    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(cfg_.host.c_str(), cfg_.port.c_str(), &hints, &res) != 0) {
        if (err) *err = "getaddrinfo failed";
        close(sock); 
        SSL_CTX_free(ctx); 
        return std::nullopt;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        if (err) *err = "connect failed";
        freeaddrinfo(res); 
        close(sock); 
        SSL_CTX_free(ctx); 
        return std::nullopt;
    }
    freeaddrinfo(res);

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);
    if (SSL_connect(ssl) <= 0) {
        if (err) *err = "SSL_connect failed";
        SSL_free(ssl); 
        close(sock); 
        SSL_CTX_free(ctx); 
        return std::nullopt;
    }

    // HTTP запрос
    std::ostringstream req;
    req << "POST /api/generate HTTP/1.1\r\n"
        << "Host: " << cfg_.host << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Connection: close\r\n";
    if (!cfg_.api_key.empty()) req << "x-api-key: " << cfg_.api_key << "\r\n";
    req << "Content-Length: " << jsonBody.size() << "\r\n\r\n"
        << jsonBody;

    const std::string request_str = req.str();
    if (SSL_write(ssl, request_str.c_str(), (int)request_str.size()) <= 0) {
        if (err) *err = "SSL_write failed";
        SSL_free(ssl); 
        close(sock); 
        SSL_CTX_free(ctx); 
        return std::nullopt;
    }

    char buf[4096];
    std::string response;
    int bytes;
    while ((bytes = SSL_read(ssl, buf, sizeof(buf)-1)) > 0) {
        buf[bytes] = '\0';
        response += buf;
    }

    SSL_free(ssl);
    close(sock);
    SSL_CTX_free(ctx);

    // Извлечение текста из JSON ответа
    auto p = response.find("\r\n\r\n");
    std::string json_part = (p != std::string::npos) ? response.substr(p + 4) : response;

    try {
        auto j = json::parse(json_part);
        if (j.contains("text")) {
            return j["text"].get<std::string>();
        }
    } catch (...) {}

    if (err) *err = "Cannot extract text from JSON response";
    return std::nullopt;
}

// Запрос к локальной LLM через libcurl
std::optional<std::string> AiAgent::sendLocalRequest(const std::string& prompt, std::string* err) {
    CURL* curl;
    CURLcode res;
    std::string response;
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    
    if(!curl) {
        if (err) *err = "curl_easy_init failed";
        return std::nullopt;
    }
    
    // Формируем URL для локального сервера
    std::string url = "http://" + cfg_.local_host + ":" + 
                     std::to_string(cfg_.local_port) + "/v1/chat/completions";
    
    // Формируем JSON-запрос в формате OpenAI API
    json payload = {
        {"model", cfg_.local_model_path.empty() ? "local-model" : cfg_.local_model_path},
        {"messages", {
            {{"role", "system"}, {"content", "You are a helpful coding assistant that analyzes code."}},
            {{"role", "user"}, {"content", prompt}}
        }},
        {"max_tokens", 800},
        {"temperature", 0.2},
        {"top_p", 0.9},
        {"stream", false}
    };
    
    std::string jsonBody = payload.dump();
    
    // Настраиваем curl
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, jsonBody.size());
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    
    // Выполняем запрос
    res = curl_easy_perform(curl);
    
    // Очистка
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    
    if(res != CURLE_OK) {
        if (err) *err = std::string("curl_easy_perform() failed: ") + 
                       curl_easy_strerror(res) + 
                       " (URL: " + url + ")";
        return std::nullopt;
    }
    
    // Парсим JSON ответ
    try {
        auto j = json::parse(response);
        
        // Проверяем наличие ошибки
        if (j.contains("error")) {
            if (err) *err = "Server error: " + j["error"].dump();
            return std::nullopt;
        }
        
        // Извлекаем текст ответа
        if (j.contains("choices") && j["choices"].is_array() && 
            !j["choices"].empty()) {
            auto choice = j["choices"][0];
            if (choice.contains("message") && 
                choice["message"].contains("content")) {
                return choice["message"]["content"].get<std::string>();
            }
        }
        
        // Альтернативный формат ответа (если сервер возвращает другой формат)
        if (j.contains("text")) {
            return j["text"].get<std::string>();
        }
        
        if (err) *err = "Unexpected response format. Expected 'choices[0].message.content' or 'text' field.";
        return std::nullopt;
        
    } catch (const std::exception& e) {
        if (err) *err = std::string("JSON parse error: ") + e.what();
        return std::nullopt;
    }
}

// Основной метод отправки запроса
std::optional<std::string> AiAgent::sendRequest(const std::string& prompt, std::string* err) {
    if (cfg_.inference_source == "local") {
        std::cout << "Использую локальную модель..." << std::endl;
        std::cout << "URL: http://" << cfg_.local_host << ":" << cfg_.local_port << "/v1/chat/completions" << std::endl;
        return sendLocalRequest(prompt, err);
    } else {
        std::cout << "Использую удаленный API..." << std::endl;
        json payload = {{"prompt", prompt}};
        std::string body = payload.dump();
        return httpsPostGenerate(body, err);
    }
}

//МЕТОДЫ ДЛЯ РАБОТЫ С БАЗОЙ ДАННЫХ

bool AiAgent::initResponseDatabase() {
    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
        std::cerr << "Не удалось открыть базу данных: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS saved_responses ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "response TEXT NOT NULL,"
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";
    
    char* err_msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Ошибка SQL: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    return true;
}

void AiAgent::closeDatabase() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool AiAgent::saveResponse(const std::string& response) {
    if (!db_ && !initResponseDatabase()) {
        std::cerr << "Не удалось инициализировать базу данных" << std::endl;
        return false;
    }
    
    const char* sql = "INSERT INTO saved_responses (response) VALUES (?);";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Ошибка подготовки SQL: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, response.c_str(), -1, SQLITE_STATIC);
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    
    if (success) {
        std::cout << "✓ Ответ сохранен" << std::endl;
    } else {
        std::cerr << "✗ Ошибка сохранения ответа: " << sqlite3_errmsg(db_) << std::endl;
    }
    
    return success;
}

bool AiAgent::enableContext() {
    if (!initResponseDatabase()) {
        std::cerr << "Не удалось инициализировать базу данных для сохранения ответов" << std::endl;
        return false;
    }
    context_enabled_ = true;
    std::cout << "✓ Контекст включен (будут сохраняться ответы ИИ)" << std::endl;
    return true;
}

bool AiAgent::disableContext() {
    context_enabled_ = false;
    closeDatabase();
    std::cout << "✓ Контекст выключен" << std::endl;
    return true;
}

std::vector<SavedResponse> AiAgent::getSavedResponses() const {
    std::vector<SavedResponse> responses;
    
    if (!db_) return responses;
    
    const char* sql = "SELECT response, timestamp FROM saved_responses ORDER BY timestamp DESC LIMIT 10;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return responses;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SavedResponse resp;
        const char* response_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* timestamp_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        
        if (response_text) resp.response = response_text;
        if (timestamp_text) resp.timestamp = timestamp_text;
        
        responses.push_back(resp);
    }
    
    sqlite3_finalize(stmt);
    return responses;
}

bool AiAgent::clearSavedResponses() {
    if (!db_) return false;
    
    const char* sql = "DELETE FROM saved_responses;";
    char* err_msg = nullptr;
    
    bool success = (sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg) == SQLITE_OK);
    
    if (err_msg) {
        sqlite3_free(err_msg);
        return false;
    }
    
    if (success) {
        std::cout << "✓ Сохраненные ответы очищены" << std::endl;
    }
    
    return success;
}

//ОСНОВНЫЕ МЕТОДЫ АНАЛИЗА КОДА

std::optional<std::string> AiAgent::analyzeCodeFile(const std::string& filepath, 
                                                   const std::string& language,
                                                   std::string* err) {
    std::string code;
    if (!readWholeFile(filepath, code, err)) {
        return std::nullopt;
    }
    
    if (code.empty()) {
        if (err) *err = "Файл пустой: " + filepath;
        return std::nullopt;
    }
    
    return analyzeCodeString(code, language, err);
}

std::optional<std::string> AiAgent::analyzeCodeString(const std::string& code,
                                                     const std::string& language,
                                                     std::string* err) {
    if (code.empty()) {
        if (err) *err = "Код пустой";
        return std::nullopt;
    }
    
    // Определяем, является ли это полным кодом (более 3 строк)
    int line_count = std::count(code.begin(), code.end(), '\n') + 1;
    bool is_complete_code = line_count > 3;
    
    std::string prompt = buildAnalysisPrompt(code, language, is_complete_code);
    auto result = sendRequest(prompt, err);
    
    if (result && context_enabled_) {
        saveResponse(*result);
    }
    
    return result;
}

//ИНТЕРАКТИВНЫЙ РЕЖИМ

void AiAgent::runInteractiveMode() {
    std::cout << "=== AI Code Analyzer - Интерактивный режим ===\n";
    std::cout << "Анализ кода на C++ и Python\n";
    std::cout << "Для выхода введите '/quit' или '/exit'\n";
    std::cout << "Для справки введите '/help'\n\n";
    
    std::cout << "Текущий источник: ";
    if (cfg_.inference_source == "local") {
        std::cout << "ЛОКАЛЬНАЯ МОДЕЛЬ" << std::endl;
        std::cout << "Сервер: " << cfg_.local_host << ":" << cfg_.local_port << std::endl;
        if (!cfg_.local_model_path.empty()) {
            std::cout << "Модель: " << cfg_.local_model_path << std::endl;
        }
    } else {
        std::cout << "УДАЛЕННЫЙ API" << std::endl;
        std::cout << "Сервер: " << cfg_.host << ":" << cfg_.port << std::endl;
    }
    std::cout << std::endl;
    
    if (context_enabled_) {
        std::cout << "✓ Контекст включен (ответы сохраняются)\n";
    } else {
        std::cout << "✗ Контекст выключен\n";
    }
    
    std::cout << "Введите код для анализа (поддерживается многострочный ввод, завершите пустой строкой):\n\n";
    
    std::string input, line;
    bool in_multiline = false;
    
    while (true) {
        if (!in_multiline) {
            std::cout << "> ";
        } else {
            std::cout << "... ";
        }
        
        std::getline(std::cin, line);
        
        // Команды
        if (!in_multiline) {
            if (line == "/quit" || line == "/exit") {
                break;
            } else if (line == "/help") {
                std::cout << "\nДоступные команды:\n";
                std::cout << "  /help          - Показать эту справку\n";
                std::cout << "  /context       - Включить/выключить сохранение ответов\n";
                std::cout << "  /saved         - Показать сохраненные ответы\n";
                std::cout << "  /clear         - Очистить сохраненные ответы\n";
                std::cout << "  /lang <язык>   - Установить язык (cpp/python/auto)\n";
                std::cout << "  /file <путь>   - Проанализировать файл\n";
                std::cout << "  /local         - Переключиться на локальную модель\n";
                std::cout << "  /remote        - Переключиться на удаленный API\n";
                std::cout << "  /model-info    - Показать информацию о модели\n";
                std::cout << "  /quit, /exit   - Выйти\n\n";
                continue;
            } else if (line == "/context") {
                if (context_enabled_) {
                    disableContext();
                } else {
                    enableContext();
                }
                continue;
            } else if (line == "/saved") {
                auto responses = getSavedResponses();
                if (responses.empty()) {
                    std::cout << "Нет сохраненных ответов\n";
                } else {
                    std::cout << "\n=== СОХРАНЕННЫЕ ОТВЕТЫ (" << responses.size() << ") ===\n";
                    for (size_t i = 0; i < responses.size(); ++i) {
                        std::cout << "[" << (i+1) << "] " << responses[i].timestamp << "\n";
                        std::cout << responses[i].response << "\n---\n";
                    }
                }
                continue;
            } else if (line == "/clear") {
                clearSavedResponses();
                continue;
            } else if (line.substr(0, 6) == "/lang ") {
                std::string lang = line.substr(6);
                std::cout << "Язык установлен: " << lang << "\n";
                continue;
            } else if (line.substr(0, 6) == "/file ") {
                std::string filepath = line.substr(6);
                std::string err;
                auto result = analyzeCodeFile(filepath, "auto", &err);
                if (result) {
                    std::cout << "\n" << *result << "\n";
                } else {
                    std::cout << "Ошибка: " << err << "\n";
                }
                continue;
            } else if (line == "/local") {
                cfg_.inference_source = "local";
                std::cout << "Переключено на ЛОКАЛЬНУЮ МОДЕЛЬ" << std::endl;
                std::cout << "Сервер: " << cfg_.local_host << ":" << cfg_.local_port << std::endl;
                continue;
            } else if (line == "/remote") {
                cfg_.inference_source = "remote";
                std::cout << "Переключено на УДАЛЕННЫЙ API" << std::endl;
                std::cout << "Сервер: " << cfg_.host << ":" << cfg_.port << std::endl;
                continue;
            } else if (line == "/model-info") {
                std::cout << "Текущая модель: ";
                if (cfg_.inference_source == "local") {
                    std::cout << "ЛОКАЛЬНАЯ" << std::endl;
                    std::cout << "Хост: " << cfg_.local_host << ":" << cfg_.local_port << std::endl;
                    std::cout << "Модель: " << (cfg_.local_model_path.empty() ? 
                        "не указана" : cfg_.local_model_path) << std::endl;
                } else {
                    std::cout << "УДАЛЕННЫЙ API" << std::endl;
                    std::cout << "Сервер: " << cfg_.host << ":" << cfg_.port << std::endl;
                }
                continue;
            }
        }
        
        // Обработка многострочного ввода
        if (line.empty()) {
            if (in_multiline && !input.empty()) {
                // Завершение многострочного ввода и анализ
                std::string err;
                auto result = analyzeCodeString(input, "auto", &err);
                if (result) {
                    std::cout << "\n" << *result << "\n";
                } else {
                    std::cout << "Ошибка: " << err << "\n";
                }
                input.clear();
                in_multiline = false;
                std::cout << "\n";
            } else if (!in_multiline) {
                // Начало многострочного ввода
                in_multiline = true;
                std::cout << "Многострочный режим (введите пустую строку для завершения):\n";
            }
        } else {
            if (in_multiline) {
                input += line + "\n";
            } else {
                // Однострочный ввод
                std::string err;
                auto result = analyzeCodeString(line, "auto", &err);
                if (result) {
                    std::cout << "\n" << *result << "\n";
                } else {
                    std::cout << "Ошибка: " << err << "\n";
                }
            }
        }
    }
    
    std::cout << "\nИнтерактивный режим завершен.\n";
}